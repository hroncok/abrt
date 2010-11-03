/*
    CCpp.cpp

    Copyright (C) 2009  Zdenek Prikryl (zprikryl@redhat.com)
    Copyright (C) 2009  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include <set>
#include "abrtlib.h"
#include "CCpp.h"
#include "abrt_exception.h"
#include "comm_layer_inner.h"
#include "Polkit.h"

using namespace std;

#define CORE_PATTERN_IFACE      "/proc/sys/kernel/core_pattern"
#define CORE_PATTERN            "|"CCPP_HOOK_PATH" "DEBUG_DUMPS_DIR" %p %s %u %c"
#define CORE_PIPE_LIMIT_IFACE   "/proc/sys/kernel/core_pipe_limit"
/* core_pipe_limit specifies how many dump_helpers might run at the same time
0 - means unlimited, but the it's not guaranteed that /proc/<pid> of crashing
process might not be available for dump_helper
4 - means that 4 dump_helpers can run at the same time, which should be enough
for ABRT, we can miss some crashes, but what are the odds that more processes
crash at the same time? This value has been recommended by nhorman
*/
#define CORE_PIPE_LIMIT "4"

#define DEBUGINFO_CACHE_DIR     LOCALSTATEDIR"/cache/abrt-di"

CAnalyzerCCpp::CAnalyzerCCpp() :
    m_bBacktrace(true),
    m_bBacktraceRemotes(false),
    m_bMemoryMap(false),
    m_bInstallDebugInfo(true),
    m_nDebugInfoCacheMB(4000),
    m_nGdbTimeoutSec(60)
{}

static void create_hash(char hash_str[SHA1_RESULT_LEN*2 + 1], const char *pInput)
{
    unsigned len;
    unsigned char hash2[SHA1_RESULT_LEN];
    sha1_ctx_t sha1ctx;

    sha1_begin(&sha1ctx);
    sha1_hash(pInput, strlen(pInput), &sha1ctx);
    sha1_end(hash2, &sha1ctx);
    len = SHA1_RESULT_LEN;

    char *d = hash_str;
    unsigned char *s = hash2;
    while (len)
    {
        *d++ = "0123456789abcdef"[*s >> 4];
        *d++ = "0123456789abcdef"[*s & 0xf];
        s++;
        len--;
    }
    *d = '\0';
    //log("hash2:%s str:'%s'", hash_str, pInput);
}

string CAnalyzerCCpp::GetGlobalUUID(const char *pDebugDumpDir)
{
    struct dump_dir *dd = dd_opendir(pDebugDumpDir, /*flags:*/ 0);
    if (!dd)
        return string("");

    if (dd_exist(dd, FILENAME_DUPHASH))
    {
        char *uuid = dd_load_text(dd, FILENAME_DUPHASH);
        dd_close(dd);
        string ret = uuid;
        free(uuid);
        return ret;
    }
    else
    {
        // Compatibility code.
        // This whole block should be deleted for Fedora 14.
        log(_("Getting global universal unique identification..."));

        string backtrace_path = concat_path_file(pDebugDumpDir, FILENAME_BACKTRACE);
        char *executable = dd_load_text(dd, FILENAME_EXECUTABLE);
        char *package = dd_load_text(dd, FILENAME_PACKAGE);
        char *uid_str = m_bBacktrace ? dd_load_text(dd, CD_UID) : xstrdup("");

        string independent_backtrace;
        if (m_bBacktrace)
        {
            /* Run abrt-backtrace to get independent backtrace suitable
               to UUID calculation. */
            char *backtrace_path = concat_path_file(pDebugDumpDir, FILENAME_BACKTRACE);
            char *args[7];
            args[0] = (char*)"abrt-backtrace";
            args[1] = (char*)"--single-thread";
            args[2] = (char*)"--remove-exit-handlers";
            args[3] = (char*)"--frame-depth=5";
            args[4] = (char*)"--remove-noncrash-frames";
            args[5] = backtrace_path;
            args[6] = NULL;

            int pipeout[2];
            xpipe(pipeout); /* stdout of abrt-backtrace */

            fflush(NULL);
            pid_t child = fork();
            if (child == -1)
                perror_msg_and_die("fork");
            if (child == 0)
            {
                VERB1 log("Executing %s", args[0]);

                xmove_fd(pipeout[1], STDOUT_FILENO);
                close(pipeout[0]); /* read side of the pipe */

                /* abrt-backtrace is executed under the user's uid and gid. */
                uid_t uid = xatoi_u(uid_str);
                struct passwd* pw = getpwuid(uid);
                gid_t gid = pw ? pw->pw_gid : uid;
                setgroups(1, &gid);
                xsetregid(gid, gid);
                xsetreuid(uid, uid);

                execvp(args[0], args);
                VERB1 perror_msg("Can't execute '%s'", args[0]);
                exit(1);
            }

            free(backtrace_path);
            close(pipeout[1]); /* write side of the pipe */

            /* Read the result from abrt-backtrace. */
            int r;
            char buff[1024];
            while ((r = safe_read(pipeout[0], buff, sizeof(buff) - 1)) > 0)
            {
                buff[r] = '\0';
                independent_backtrace += buff;
            }
            close(pipeout[0]);

            /* Wait until it exits, and check the exit status. */
            errno = 0;
            int status;
            waitpid(child, &status, 0);
            if (!WIFEXITED(status))
            {
                perror_msg("abrt-backtrace not executed properly, "
                           "status: %x signal: %d", status, WIFSIGNALED(status));
            }
            else
            {
                int exit_status = WEXITSTATUS(status);
                if (exit_status == 79) /* EX_PARSINGFAILED */
                {
                    /* abrt-backtrace returns alternative backtrace
                       representation in this case, so everything will work
                       as expected except worse duplication detection */
                    log_msg("abrt-backtrace failed to parse the backtrace");
                }
                else if (exit_status == 80) /* EX_THREADDETECTIONFAILED */
                {
                    /* abrt-backtrace returns backtrace with all threads
                       in this case, so everything will work as expected
                       except worse duplication detection */
                    log_msg("abrt-backtrace failed to determine crash frame");
                }
                else if (exit_status != 0)
                {
                    /* this is unexpected problem and it should be investigated */
                    error_msg("abrt-backtrace run failed, exit value: %d",
                              exit_status);
                }
            }

            /*VERB1 log("abrt-backtrace result: %s", independent_backtrace.c_str());*/
        }
        /* else: no backtrace, independent_backtrace == ""
           no backtrace => rating = 0
        */
        else
        {
            dd_save_text(dd, FILENAME_RATING, "0");
        }
        dd_close(dd);

        char *hash_str = xasprintf("%s%s%s", package, executable, independent_backtrace.c_str());
        free(package);
        free(executable);

        char hash_str2[SHA1_RESULT_LEN*2 + 1];
        create_hash(hash_str2, hash_str);
        free(hash_str);

        return hash_str2;
    }
}

/*
 this is just a workaround until kernel changes it's behavior
 when handling pipes in core_pattern
*/
#ifdef HOSTILE_KERNEL
#define CORE_SIZE_PATTERN "Max core file size=1:unlimited"
static int isdigit_str(char *str)
{
    do {
        if (*str < '0' || *str > '9')
            return 0;
    } while (*++str);
    return 1;
}

static int set_limits()
{
    DIR *dir = opendir("/proc");
    if (!dir) {
        /* this shouldn't fail, but to be safe.. */
        return 1;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!isdigit_str(ent->d_name))
            continue;

        char limits_name[sizeof("/proc/%s/limits") + sizeof(long)*3];
        snprintf(limits_name, sizeof(limits_name), "/proc/%s/limits", ent->d_name);
        FILE *limits_fp = fopen(limits_name, "r");
        if (!limits_fp) {
            break;
        }

        char line[128];
        char *ulimit_c = NULL;
        while (1) {
            if (fgets(line, sizeof(line)-1, limits_fp) == NULL)
                break;
            if (strncmp(line, "Max core file size", sizeof("Max core file size")-1) == 0) {
                ulimit_c = skip_whitespace(line + sizeof("Max core file size")-1);
                skip_non_whitespace(ulimit_c)[0] = '\0';
                break;
            }
        }
        fclose(limits_fp);
        if (!ulimit_c || ulimit_c[0] != '0' || ulimit_c[1] != '\0') {
            /*process has nonzero ulimit -c, so need to modify it*/
            continue;
        }
        /* echo -n 'Max core file size=1:unlimited' >/proc/PID/limits */
        int fd = open(limits_name, O_WRONLY);
        if (fd >= 0) {
            errno = 0;
            /*full_*/
            ssize_t n = write(fd, CORE_SIZE_PATTERN, sizeof(CORE_SIZE_PATTERN)-1);
            if (n < sizeof(CORE_SIZE_PATTERN)-1)
                log("warning: can't write core_size limit to: %s", limits_name);
            close(fd);
        }
        else
        {
            log("warning: can't open %s for writing", limits_name);
        }
    }
    closedir(dir);
    return 0;
}
#endif /* HOSTILE_KERNEL */

void CAnalyzerCCpp::Init()
{
    FILE *fp = fopen(CORE_PATTERN_IFACE, "r");
    if (fp)
    {
        char line[PATH_MAX];
        if (fgets(line, sizeof(line), fp))
            m_sOldCorePattern = line;
        fclose(fp);
    }
    if (m_sOldCorePattern[0] == '|')
    {
        if (m_sOldCorePattern == CORE_PATTERN)
        {
            log("warning: %s already contains %s, "
                "did abrt daemon crash recently?",
                CORE_PATTERN_IFACE, CORE_PATTERN);
            /* There is no point in "restoring" CORE_PATTERN_IFACE
             * to CORE_PATTERN on exit. Will restore to a default value:
             */
            m_sOldCorePattern = "core";
        } else {
            log("warning: %s was already set to run a crash analyser (%s), "
                "abrt may interfere with it",
                CORE_PATTERN_IFACE, CORE_PATTERN);
        }
    }
#ifdef HOSTILE_KERNEL
    if (set_limits() != 0)
        log("warning: failed to set core_size limit, ABRT won't detect crashes in"
            "compiled apps");
#endif

    fp = fopen(CORE_PATTERN_IFACE, "w");
    if (fp)
    {
        fputs(CORE_PATTERN, fp);
        fclose(fp);
    }

    /* read the core_pipe_limit and change it if it's == 0
       otherwise the abrt-hook-ccpp won't be able to read /proc/<pid>
       of the crashing process
    */
    fp = fopen(CORE_PIPE_LIMIT_IFACE, "r");
    if (fp)
    {
        /* we care only about the first char, if it's
         * not '0' then we don't have to change it,
         * because it means that it's already != 0
         */
        char pipe_limit[2];
        if (!fgets(pipe_limit, sizeof(pipe_limit), fp))
            pipe_limit[0] = '1'; /* not 0 */
        fclose(fp);
        if (pipe_limit[0] == '0')
        {
            fp = fopen(CORE_PIPE_LIMIT_IFACE, "w");
            if (fp)
            {
                fputs(CORE_PIPE_LIMIT, fp);
                fclose(fp);
            }
            else
            {
                log("warning: failed to set core_pipe_limit, ABRT won't detect"
                    "crashes in compiled apps if kernel > 2.6.31");
            }
        }
    }
}

void CAnalyzerCCpp::DeInit()
{
    /* no need to restore the core_pipe_limit, because it's only used
       when there is s pipe in core_pattern
    */
    FILE *fp = fopen(CORE_PATTERN_IFACE, "w");
    if (fp)
    {
        fputs(m_sOldCorePattern.c_str(), fp);
        fclose(fp);
    }
}

void CAnalyzerCCpp::SetSettings(const map_plugin_settings_t& pSettings)
{
    m_pSettings = pSettings;

    map_plugin_settings_t::const_iterator end = pSettings.end();
    map_plugin_settings_t::const_iterator it;
    it = pSettings.find("Backtrace");
    if (it != end)
    {
        m_bBacktrace = string_to_bool(it->second.c_str());
    }
    it = pSettings.find("BacktraceRemotes");
    if (it != end)
    {
        m_bBacktraceRemotes = string_to_bool(it->second.c_str());
    }
    it = pSettings.find("MemoryMap");
    if (it != end)
    {
        m_bMemoryMap = string_to_bool(it->second.c_str());
    }
    it = pSettings.find("DebugInfo");
    if (it != end)
    {
        m_sDebugInfo = it->second;
    }
    it = pSettings.find("DebugInfoCacheMB");
    if (it != end)
    {
        m_nDebugInfoCacheMB = xatou(it->second.c_str());
    }
    it = pSettings.find("GdbTimeoutSec");
    if (it != end)
    {
        m_nGdbTimeoutSec = xatoi_u(it->second.c_str());
    }
    it = pSettings.find("InstallDebugInfo");
    if (it == end) //compat, remove after 0.0.11
        it = pSettings.find("InstallDebuginfo");
    if (it != end)
    {
        m_bInstallDebugInfo = string_to_bool(it->second.c_str());
    }
    m_sDebugInfoDirs = DEBUGINFO_CACHE_DIR;
    it = pSettings.find("ReadonlyLocalDebugInfoDirs");
    if (it != end)
    {
        m_sDebugInfoDirs += ':';
        m_sDebugInfoDirs += it->second;
    }
}

//ok to delete?
//const map_plugin_settings_t& CAnalyzerCCpp::GetSettings()
//{
//    m_pSettings["MemoryMap"] = m_bMemoryMap ? "yes" : "no";
//    m_pSettings["DebugInfo"] = m_sDebugInfo;
//    m_pSettings["DebugInfoCacheMB"] = to_string(m_nDebugInfoCacheMB);
//    m_pSettings["InstallDebugInfo"] = m_bInstallDebugInfo ? "yes" : "no";
//
//    return m_pSettings;
//}

PLUGIN_INFO(ANALYZER,
            CAnalyzerCCpp,
            "CCpp",
            "0.0.1",
            _("Analyzes crashes in C/C++ programs"),
            "zprikryl@redhat.com",
            "https://fedorahosted.org/abrt/wiki",
            "");
