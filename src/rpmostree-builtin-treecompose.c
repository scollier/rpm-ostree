/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <string.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>

#include "rpmostree-builtins.h"
#include "rpmostree-postprocess.h"

#include "libgsystem.h"

static char *opt_workdir;

static GOptionEntry option_entries[] = {
  { "workdir", 0, 0, G_OPTION_ARG_STRING, &opt_workdir, "Working directory", "REPO" },
  { NULL }
};

static char *
subprocess_context_print_args (GSSubprocessContext   *ctx)
{
  GString *ret = g_string_new ("");
  gs_strfreev char **argv = NULL;
  char **strviter;

  g_object_get ((GObject*)ctx, "argv", &argv, NULL);
  for (strviter = argv; strviter && *strviter; strviter++)
    {
      gs_free char *quoted = g_shell_quote (*strviter);
      g_string_append (ret, quoted);
    }

  return g_string_free (ret, FALSE);
}

static const char *
object_require_string_member (JsonObject     *object,
                              const char     *member_name,
                              GError        **error)
{
  const char *ret = json_object_get_string_member (object, member_name);
  if (!ret)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No member '%s' found", member_name);
      return NULL;
    }
  return ret;
}

static gboolean
object_get_optional_string_member (JsonObject     *object,
                                   const char     *member_name,
                                   const char    **out_value,
                                   GError        **error)
{
  gboolean ret = FALSE;
  JsonNode *node = json_object_get_member (object, member_name);

  if (node != NULL)
    {
      *out_value = json_node_get_string (node);
      if (!*out_value)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Member '%s' is not a string", member_name);
          goto out;
        }
    }
  else
    *out_value = NULL;

  ret = TRUE;
 out:
  return ret;
}

static const char *
array_require_string_element (JsonArray      *array,
                              guint           i,
                              GError        **error)
{
  const char *ret = json_array_get_string_element (array, i);
  if (!ret)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Element at index %u is not a string", i);
      return NULL;
    }
  return ret;
}

static gboolean
append_string_array_to (JsonObject   *object,
                        const char   *member_name,
                        GPtrArray    *array,
                        GError      **error)
{
  JsonArray *jarray = json_object_get_array_member (object, member_name);
  guint i, len;

  if (!jarray)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No member '%s' found", member_name);
      return FALSE;
    }

  len = json_array_get_length (jarray);
  for (i = 0; i < len; i++)
    {
      const char *v = array_require_string_element (jarray, i, error);
      if (!v)
        return FALSE;
      g_ptr_array_add (array, g_strdup (v));
    }

  return TRUE;
}

static gboolean
replace_nsswitch (GFile         *target_usretc,
                  GCancellable  *cancellable,
                  GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *nsswitch_conf =
    g_file_get_child (target_usretc, "nsswitch.conf");
  gs_free char *nsswitch_contents = NULL;
  gs_free char *new_nsswitch_contents = NULL;

  static gsize regex_initialized;
  static GRegex *passwd_regex;

  if (g_once_init_enter (&regex_initialized))
    {
      passwd_regex = g_regex_new ("^(passwd|group):\\s+files(.*)$",
                                  G_REGEX_MULTILINE, 0, NULL);
      g_assert (passwd_regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  nsswitch_contents = gs_file_load_contents_utf8 (nsswitch_conf, cancellable, error);
  if (!nsswitch_contents)
    goto out;

  new_nsswitch_contents = g_regex_replace (passwd_regex,
                                           nsswitch_contents, -1, 0,
                                           "\\1: files altfiles\\2",
                                           0, error);
  if (!new_nsswitch_contents)
    goto out;

  if (!g_file_replace_contents (nsswitch_conf, new_nsswitch_contents,
                                strlen (new_nsswitch_contents),
                                NULL, FALSE, 0, NULL,
                                cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

typedef struct {
  GSSubprocess *process;
  GFile *reposdir_path;
  GFile *tmp_reposdir_path;
  GDataOutputStream *stdin;
  /* GDataInputStream *stdout; */
} YumContext;

static gboolean
yum_context_close (YumContext   *yumctx,
                   GCancellable *cancellable,
                   GError      **error)
{
  gboolean ret = FALSE;

  if (!yumctx)
    return TRUE;

  if (yumctx->tmp_reposdir_path)
    {
      if (!gs_file_rename (yumctx->tmp_reposdir_path, yumctx->reposdir_path,
                           cancellable, error))
        goto out;
      g_clear_object (&yumctx->reposdir_path);
      g_clear_object (&yumctx->tmp_reposdir_path);
    }
  
  if (yumctx->process)
    {
      if (yumctx->stdin)
        {
          if (!g_output_stream_close ((GOutputStream*)yumctx->stdin, cancellable, error))
            goto out;
          g_clear_object (&yumctx->stdin);
        }
      /*
      if (yumctx->stdout)
        {
          if (!g_input_stream_close ((GInputStream*)yumctx->stdout, cancellable, error))
            goto out;
          g_clear_object (&yumctx->stdout);
        }
      */
      
      g_print ("Waiting for yum...\n");
      if (!gs_subprocess_wait_sync_check (yumctx->process, cancellable, error))
        goto out;
      g_print ("Waiting for yum [OK]\n");
      g_clear_object (&yumctx->process);
    }

  ret = TRUE;
 out:
  return ret;
}

static void
yum_context_free (YumContext  *yumctx)
{
  if (!yumctx)
    return;
  (void) yum_context_close (yumctx, NULL, NULL);
  g_free (yumctx);
}

static inline
void cleanup_keyfile_unref (void *loc)
{
  GKeyFile *locp = *((GKeyFile**)loc);
  if (locp)
    g_key_file_unref (locp);
}

static gboolean
append_repo_and_cache_opts (JsonObject *treedata,
                            GFile      *cachedir,
                            GPtrArray  *args,
                            GCancellable *cancellable,
                            GError    **error)
{
  gboolean ret = FALSE;
  JsonArray *enable_repos = NULL;
  JsonArray *repos_data = NULL;
  gs_unref_object GFile *yumcache_lookaside = NULL;
  gs_unref_object GFile *repos_tmpdir = NULL;

  yumcache_lookaside = g_file_resolve_relative_path (cachedir, "yum-cache");
  if (!gs_file_ensure_directory (yumcache_lookaside, TRUE, cancellable, error))
    goto out;

  repos_tmpdir = g_file_resolve_relative_path (cachedir, "tmp-repos");
  if (!gs_shutil_rm_rf (repos_tmpdir, cancellable, error))
    goto out;
  if (!gs_file_ensure_directory (repos_tmpdir, TRUE, cancellable, error))
    goto out;

  if (g_getenv ("RPM_OSTREE_OFFLINE"))
    g_ptr_array_add (args, g_strdup ("-C"));

  g_ptr_array_add (args, g_strdup ("--disablerepo=*"));

  if (json_object_has_member (treedata, "repos"))
    enable_repos = json_object_get_array_member (treedata, "repos");
  if (enable_repos)
    {
      guint i;
      guint n = json_array_get_length (enable_repos);
      for (i = 0; i < n; i++)
        {
          const char *reponame = array_require_string_element (enable_repos, i, error);
          if (!reponame)
            goto out;
          g_ptr_array_add (args, g_strconcat ("--enablerepo=", reponame, NULL));
        }
    }

  if (json_object_has_member (treedata, "repos_data"))
    repos_data = json_object_get_array_member (treedata, "repos_data");
  else
    repos_data = NULL;
  if (repos_data)
    {
      guint i;
      guint n = json_array_get_length (repos_data);

      if (n > 0)
        g_ptr_array_add (args, g_strconcat ("--setopt=reposdir=/etc/yum.repos.d,",
                                            gs_file_get_path_cached (repos_tmpdir),
                                            NULL));

      for (i = 0; i < n; i++)
        {
          const char *repodata = array_require_string_element (repos_data, i, error);
          __attribute__ ((cleanup(cleanup_keyfile_unref))) GKeyFile *keyfile = NULL;
          gs_strfreev char **groups = NULL;
          const char *reponame;
          gs_free char *rpmostree_reponame = NULL;
          gs_free char *rpmostree_repo_filename = NULL;
          gs_unref_object GFile *repo_tmp_file = NULL;
          gsize len;

          if (!repodata)
            goto out;

          keyfile = g_key_file_new ();
          if (!g_key_file_load_from_data (keyfile, repodata, -1, 0, error))
            {
              g_prefix_error (error, "Parsing keyfile data in repos_data: ");
              goto out;
            }

          groups = g_key_file_get_groups (keyfile, &len);
          if (len == 0)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "No groups found in keyfile data in repos_data");
              goto out;
            }

          reponame = groups[0];
          g_assert (strchr (reponame, '/') == NULL);
          rpmostree_reponame = g_strconcat (reponame, NULL);
          rpmostree_repo_filename = g_strconcat (rpmostree_reponame, ".repo", NULL);
          repo_tmp_file = g_file_get_child (repos_tmpdir, rpmostree_repo_filename);

          if (!g_file_replace_contents (repo_tmp_file, repodata, strlen (repodata),
                                        NULL, FALSE, 0,
                                        NULL,
                                        cancellable, error))
            goto out;
          
          g_ptr_array_add (args, g_strconcat ("--enablerepo=", rpmostree_reponame, NULL));
        }
    }

  g_ptr_array_add (args, g_strdup ("--setopt=keepcache=1"));
  g_ptr_array_add (args, g_strconcat ("--setopt=cachedir=",
                                      gs_file_get_path_cached (yumcache_lookaside),
                                      NULL));

  ret = TRUE;
 out:
  return ret;
}

static YumContext *
yum_context_new (JsonObject     *treedata,
                 GFile          *yumroot,
                 GFile          *cachedir,
                 GCancellable   *cancellable,
                 GError        **error)
{
  gboolean success = FALSE;
  YumContext *yumctx = NULL;
  GPtrArray *yum_argv = g_ptr_array_new_with_free_func (g_free);
  gs_unref_object GSSubprocessContext *context = NULL;
  gs_unref_object GSSubprocess *yum_process = NULL;
  gs_unref_object GFile *reposdir_path = NULL;

  g_ptr_array_add (yum_argv, g_strdup ("yum"));
  g_ptr_array_add (yum_argv, g_strdup ("-y"));

  if (!append_repo_and_cache_opts (treedata, cachedir, yum_argv,
                                   cancellable, error))
    goto out;

  g_ptr_array_add (yum_argv, g_strconcat ("--installroot=",
                                          gs_file_get_path_cached (yumroot),
                                          NULL));
  
  g_ptr_array_add (yum_argv, g_strdup ("shell"));

  g_ptr_array_add (yum_argv, NULL);

  context = gs_subprocess_context_new ((char**)yum_argv->pdata);
  {
    gs_strfreev char **duped_environ = g_get_environ ();

    duped_environ = g_environ_setenv (duped_environ, "OSTREE_KERNEL_INSTALL_NOOP", "1", TRUE);
    /* See fedora's kernel.spec */
    duped_environ = g_environ_setenv (duped_environ, "HARDLINK", "no", TRUE);

    gs_subprocess_context_set_environment (context, duped_environ);
  }

  reposdir_path = g_file_resolve_relative_path (yumroot, "etc/yum.repos.d");
  /* Hideous workaround for the fact that as soon as yum.repos.d
     exists in the install root, yum will prefer it. */
  if (g_file_query_exists (reposdir_path, NULL))
    {
      yumctx->reposdir_path = g_object_ref (reposdir_path);
      yumctx->tmp_reposdir_path = g_file_resolve_relative_path (yumroot, "etc/yum.repos.d.tmp");
      if (!gs_file_rename (reposdir_path, yumctx->tmp_reposdir_path,
                           cancellable, error))
        goto out;
    }

  gs_subprocess_context_set_stdin_disposition (context, GS_SUBPROCESS_STREAM_DISPOSITION_PIPE);
  /* gs_subprocess_context_set_stdout_disposition (context, GS_SUBPROCESS_STREAM_DISPOSITION_PIPE); */

  yumctx = g_new0 (YumContext, 1);

  {
    gs_free char *cmdline = subprocess_context_print_args (context);
    g_print ("Starting %s\n", cmdline);
  }
  yumctx->process = gs_subprocess_new (context, cancellable, error);
  if (!yumctx->process)
    goto out;

  yumctx->stdin = (GDataOutputStream*)g_data_output_stream_new (gs_subprocess_get_stdin_pipe (yumctx->process));
  /* yumctx->stdout = (GDataInputStream*)g_data_input_stream_new (gs_subprocess_get_stdout_pipe (yumctx->process)); */

  success = TRUE;
 out:
  if (!success)
    {
      yum_context_free (yumctx);
      return NULL;
    }
  return yumctx;
}

static gboolean
yum_context_command (YumContext   *yumctx,
                     const char   *cmd,
                     GPtrArray   **out_lines,
                     GCancellable *cancellable,
                     GError      **error)
{
  gboolean ret = FALSE;
  gsize bytes_written;
  gs_unref_ptrarray GPtrArray *lines = g_ptr_array_new_with_free_func (g_free);
  gs_free char *cmd_nl = g_strconcat (cmd, "\n", NULL);

  g_print ("yum> %s", cmd_nl);
  if (!g_output_stream_write_all ((GOutputStream*)yumctx->stdin,
                                  cmd_nl, strlen (cmd_nl), &bytes_written,
                                  cancellable, error))
    goto out;

  ret = TRUE;
  gs_transfer_out_value (out_lines, &lines);
 out:
  return ret;
}
                  
static gboolean
yuminstall (JsonObject      *treedata,
            GFile           *yumroot,
            GFile           *cachedir,
            char           **packages,
            GCancellable    *cancellable,
            GError         **error)
{
  gboolean ret = FALSE;
  char **strviter;
  YumContext *yumctx;

  yumctx = yum_context_new (treedata, yumroot, cachedir, cancellable, error);
  if (!yumctx)
    goto out;

  for (strviter = packages; strviter && *strviter; strviter++)
    {
      gs_free char *cmd = NULL;
      const char *package = *strviter;
      gs_unref_ptrarray GPtrArray *lines = NULL;

      if (g_str_has_prefix (package, "@"))
        cmd = g_strconcat ("group install ", package, NULL);
      else
        cmd = g_strconcat ("install ", package, NULL);
        
      if (!yum_context_command (yumctx, cmd, &lines,
                                cancellable, error))
        goto out;
    }

  {
    gs_unref_ptrarray GPtrArray *lines = NULL;
    if (!yum_context_command (yumctx, "run", &lines,
                              cancellable, error))
      goto out;
  }

  if (!yum_context_close (yumctx, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
rpmostree_builtin_treecompose (int             argc,
                               char          **argv,
                               GCancellable   *cancellable,
                               GError        **error)
{
  gboolean ret = FALSE;
  GOptionContext *context = g_option_context_new ("- Run yum and commit the result to an OSTree repository");
  const char *ref;
  JsonNode *treefile_root = NULL;
  JsonObject *treefile = NULL;
  JsonArray *internal_postprocessing = NULL;
  JsonArray *units = NULL;
  guint len;
  gs_free char *ref_unix = NULL;
  gs_unref_object GFile *cachedir = NULL;
  gs_unref_object GFile *yumroot = NULL;
  gs_unref_object GFile *targetroot = NULL;
  gs_unref_object GFile *yumroot_varcache = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_ptrarray GPtrArray *bootstrap_packages = NULL;
  gs_unref_ptrarray GPtrArray *packages = NULL;
  gs_unref_object GFile *treefile_path = NULL;
  gs_unref_object JsonParser *treefile_parser = NULL;
  
  g_option_context_add_main_entries (context, option_entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      g_printerr ("usage: " PACKAGE_STRING " create TREEFILE\n");
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Option processing failed");
      goto out;
    }
  
  treefile_path = g_file_new_for_path (argv[1]);

  if (opt_workdir && chdir (opt_workdir) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to chdir to '%s': %s",
                   opt_workdir, strerror (errno));
      goto out;
    }

  treefile_parser = json_parser_new ();
  if (!json_parser_load_from_file (treefile_parser,
                                   gs_file_get_path_cached (treefile_path),
                                   error))
    goto out;

  treefile_root = json_parser_get_root (treefile_parser);
  if (!JSON_NODE_HOLDS_OBJECT (treefile_root))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Treefile root is not an object");
      goto out;
    }
  treefile = json_node_get_object (treefile_root);

  cachedir = g_file_new_for_path ("cache");
  if (!gs_file_ensure_directory (cachedir, TRUE, cancellable, error))
    goto out;

  yumroot = g_file_get_child (cachedir, "yum");
  if (!gs_shutil_rm_rf (yumroot, cancellable, error))
    goto out;
  targetroot = g_file_resolve_relative_path (cachedir, "rootfs");

  ref = object_require_string_member (treefile, "ref", error);
  if (!ref)
    goto out;

  ref_unix = g_strdelimit (g_strdup (ref), "/", '_');

  bootstrap_packages = g_ptr_array_new ();
  packages = g_ptr_array_new ();

  if (!append_string_array_to (treefile, "bootstrap_packages", bootstrap_packages, error))
    goto out;
  g_ptr_array_add (bootstrap_packages, NULL);

  if (!append_string_array_to (treefile, "packages", packages, error))
    goto out;
  g_ptr_array_add (packages, NULL);
    

  {
    gs_free char *cached_packageset_name =
      g_strconcat ("packageset-", ref_unix, ".txt", NULL);
    gs_unref_object GFile *rpmtextlist_path = 
      g_file_resolve_relative_path (cachedir, cached_packageset_name);
    gs_free char *cached_packageset_name_new =
      g_strconcat (cached_packageset_name, ".new", NULL);
    gs_unref_object GFile *rpmtextlist_path_new = 
      g_file_resolve_relative_path (cachedir, cached_packageset_name_new);
    GPtrArray *repoquery_argv = g_ptr_array_new_with_free_func (g_free);
    gs_unref_object GSSubprocessContext *repoquery_proc_ctx = NULL;
    gs_unref_object GSSubprocess *repoquery_proc = NULL;
    guint i;
    GString *repoquery_arg_string = NULL;
    gboolean first = TRUE;

    g_ptr_array_add (repoquery_argv, g_strdup (PKGLIBDIR "/repoquery-sorted"));
    if (!append_repo_and_cache_opts (treefile, cachedir, repoquery_argv,
                                     cancellable, error))
      goto out;
    g_ptr_array_add (repoquery_argv, g_strdup ("--recursive"));
    g_ptr_array_add (repoquery_argv, g_strdup ("--requires"));
    g_ptr_array_add (repoquery_argv, g_strdup ("--resolve"));

    repoquery_arg_string = g_string_new ("");

    for (i = 0; i < packages->len; i++)
      {
        const char *package = packages->pdata[i];
        if (!package)
          continue;

        g_ptr_array_add (repoquery_argv, g_strdup (package));
        if (first)
          first = FALSE;
        else
          g_string_append_c (repoquery_arg_string, ' ');
        g_string_append (repoquery_arg_string, package);
      }

    g_print ("Running repoquery: %s\n", repoquery_arg_string->str);
    g_string_free (repoquery_arg_string, TRUE);

    g_ptr_array_add (repoquery_argv, NULL);
      
    repoquery_proc_ctx = gs_subprocess_context_new ((char**)repoquery_argv->pdata);
    gs_subprocess_context_set_stdout_file_path (repoquery_proc_ctx, gs_file_get_path_cached (rpmtextlist_path_new));
    gs_subprocess_context_set_stderr_disposition (repoquery_proc_ctx, GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT);
    g_print ("Resolving dependencies...\n");
    repoquery_proc = gs_subprocess_new (repoquery_proc_ctx, cancellable, error);
    if (!repoquery_proc)
      goto out;

    if (!gs_subprocess_wait_sync_check (repoquery_proc, cancellable, error))
      goto out;
    
    if (g_file_query_exists (rpmtextlist_path, NULL))
      {
        GError *temp_error = NULL;
        gs_unref_object GSSubprocess *diff_proc = NULL;
        gboolean differs = FALSE;

        g_print ("Comparing diff of previous tree\n");
        diff_proc =
          gs_subprocess_new_simple_argl (GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                         GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                         cancellable,
                                         error,
                                         "diff", "-u",
                                         gs_file_get_path_cached (rpmtextlist_path),
                                         gs_file_get_path_cached (rpmtextlist_path_new),
                                         NULL);

        if (!gs_subprocess_wait_sync_check (diff_proc, cancellable, &temp_error))
          {
            int ecode;
            if (temp_error->domain != G_SPAWN_EXIT_ERROR)
              {
                g_propagate_error (error, temp_error);
                goto out;
              }
            ecode = temp_error->code;
            g_assert (ecode != 0);
            if (ecode == 1)
              differs = TRUE;
            else
              {
                g_propagate_error (error, temp_error);
                goto out;
              }
          }

        if (!differs)
          {
            g_print ("No changes in package set\n");
            if (!gs_file_unlink (rpmtextlist_path_new, cancellable, error))
              goto out;
            goto out;
          }
      }
    else
      {
        g_print ("No previous diff file found at '%s'\n",
                 gs_file_get_path_cached (rpmtextlist_path));
      }

    /* Ensure we have enough to modify NSS */
    if (!yuminstall (treefile, yumroot, cachedir,
                     (char**)bootstrap_packages->pdata,
                     cancellable, error))
      goto out;

    /* Prepare NSS configuration; this needs to be done
       before any invocations of "useradd" in %post */

    {
      gs_unref_object GFile *yumroot_passwd =
        g_file_resolve_relative_path (yumroot, "usr/lib/passwd");
      gs_unref_object GFile *yumroot_group =
        g_file_resolve_relative_path (yumroot, "usr/lib/group");
      gs_unref_object GFile *yumroot_etc = 
        g_file_resolve_relative_path (yumroot, "etc");

      if (!g_file_replace_contents (yumroot_passwd, "", 0, NULL, FALSE, 0,
                                    NULL, cancellable, error))
        goto out;
      if (!g_file_replace_contents (yumroot_group, "", 0, NULL, FALSE, 0,
                                    NULL, cancellable, error))
        goto out;

      if (!replace_nsswitch (yumroot_etc, cancellable, error))
        goto out;
    }

    {
      if (!yuminstall (treefile, yumroot, cachedir,
                       (char**)packages->pdata,
                       cancellable, error))
        goto out;
    }

    ref_unix = g_strdelimit (g_strdup (ref), "/", '_');

    if (g_strcmp0 (g_getenv ("RPM_OSTREE_BREAK"), "post-yum") == 0)
      goto out;

    if (!rpmostree_postprocess (yumroot, cancellable, error))
      goto out;

    if (json_object_has_member (treefile, "postprocess"))
      internal_postprocessing = json_object_get_array_member (treefile, "postprocess");

    if (internal_postprocessing)
      len = json_array_get_length (internal_postprocessing);
    else
      len = 0;

    for (i = 0; i < len; i++)
      {
        gs_unref_object GFile *pkglibdir = g_file_new_for_path (PKGLIBDIR);
        gs_unref_object GFile *pkglibdir_posts = g_file_get_child (pkglibdir, "postprocessing");
        gs_unref_object GFile *post_path = NULL;
        const char *post_name = array_require_string_element (internal_postprocessing, i, error);

        if (!post_name)
          goto out;

        post_path = g_file_get_child (pkglibdir_posts, post_name);

        g_print ("Running internal postprocessing command '%s'\n",
                 gs_file_get_path_cached (post_path));
        if (!gs_subprocess_simple_run_sync (gs_file_get_path_cached (yumroot),
                                            GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
                                            cancellable, error,
                                            gs_file_get_path_cached (post_path),
                                            NULL))
          goto out;
      }

    if (json_object_has_member (treefile, "units"))
      units = json_object_get_array_member (treefile, "units");

    if (units)
      len = json_array_get_length (units);
    else
      len = 0;

    {
      gs_unref_object GFile *multiuser_wants_dir =
        g_file_resolve_relative_path (yumroot, "usr/etc/systemd/system/multi-user.target.wants");

      if (!gs_file_ensure_directory (multiuser_wants_dir, TRUE, cancellable, error))
        goto out;

      for (i = 0; i < len; i++)
        {
          const char *unitname = array_require_string_element (units, i, error);
          gs_unref_object GFile *unit_link_target = NULL;
          gs_free char *symlink_target = NULL;

          if (!unitname)
            goto out;

          symlink_target = g_strconcat ("/usr/lib/systemd/system/", unitname, NULL);
          unit_link_target = g_file_get_child (multiuser_wants_dir, unitname);

          if (g_file_query_file_type (unit_link_target, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_SYMBOLIC_LINK)
            continue;
          
          g_print ("Adding %s to multi-user.target.wants\n", unitname);

          if (!g_file_make_symbolic_link (unit_link_target, symlink_target,
                                          cancellable, error))
            goto out;
        }
    }

    {
      gs_unref_object GFile *target_treefile_dir_path =
        g_file_resolve_relative_path (yumroot, "usr/share/rpm-ostree");
      gs_unref_object GFile *target_treefile_path =
        g_file_get_child (target_treefile_dir_path, "treefile.json");
      
      if (!gs_file_ensure_directory (target_treefile_dir_path, TRUE,
                                     cancellable, error))
        goto out;
                                     
      g_print ("Copying '%s' to '%s'\n",
               gs_file_get_path_cached (treefile_path),
               gs_file_get_path_cached (target_treefile_path));
      if (!g_file_copy (treefile_path, target_treefile_path,
                        G_FILE_COPY_TARGET_DEFAULT_PERMS,
                        cancellable, NULL, NULL, error))
        goto out;
    }
  
    {
      gs_unref_object GFile *repo_path = g_file_new_for_path ("repo");
      repo = ostree_repo_new (repo_path);
    }
    
    if (!ostree_repo_open (repo, cancellable, error))
      goto out;

    
    {
      const char *gpgkey;
      if (!object_get_optional_string_member (treefile, "gpg_key", &gpgkey, error))
        goto out;

      if (!rpmostree_commit (yumroot, repo, ref, gpgkey,
                             json_object_get_boolean_member (treefile, "selinux"),
                             cancellable, error))
      goto out;
    }

    if (!gs_file_rename (rpmtextlist_path_new, rpmtextlist_path,
                         cancellable, error))
      goto out;
  }

  g_print ("Complete\n");
  
 out:
  return ret;
}
