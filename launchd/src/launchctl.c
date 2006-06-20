/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

static const char *const __rcs_file_version__ = "$Revision: 1.85 $";

#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPriv.h>
#include <NSSystemDirectories.h>
#include <mach/mach.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/fcntl.h>
#include <sys/event.h>
#include <sys/resource.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet6/nd6.h>
#include <unistd.h>
#include <dirent.h>
#include <libgen.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <grp.h>
#include <netdb.h>
#include <syslog.h>
#include <glob.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <dns_sd.h>
#include <paths.h>
#include <utmp.h>
#include <utmpx.h>

#include "bootstrap_public.h"
#include "bootstrap_private.h"
#include "launch.h"
#include "launch_priv.h"

#define LAUNCH_SECDIR "/tmp/launch-XXXXXX"

#define MACHINIT_JOBKEY_ONDEMAND	"OnDemand"
#define MACHINIT_JOBKEY_SERVICENAME	"ServiceName"
#define MACHINIT_JOBKEY_COMMAND		"Command"
#define MACHINIT_JOBKEY_SERVERPORT	"ServerPort"
#define MACHINIT_JOBKEY_SERVICEPORT	"ServicePort"

#define assumes(e)      \
	        (__builtin_expect(!(e), 0) ? _log_launchctl_bug(__rcs_file_version__, __FILE__, __LINE__, #e), false : true)


struct load_unload_state {
	launch_data_t pass0;
	launch_data_t pass1;
	launch_data_t pass2;
	char *session_type;
	unsigned int editondisk:1, load:1, forceload:1, __pad:29;
};

static void myCFDictionaryApplyFunction(const void *key, const void *value, void *context);
static bool launch_data_array_append(launch_data_t a, launch_data_t o);
static void distill_jobs(launch_data_t);
static void distill_config_file(launch_data_t);
static void sock_dict_cb(launch_data_t what, const char *key, void *context);
static void sock_dict_edit_entry(launch_data_t tmp, const char *key, launch_data_t fdarray, launch_data_t thejob);
static launch_data_t CF2launch_data(CFTypeRef);
static launch_data_t read_plist_file(const char *file, bool editondisk, bool load);
static CFPropertyListRef CreateMyPropertyListFromFile(const char *);
static void WriteMyPropertyListToFile(CFPropertyListRef, const char *);
static void readpath(const char *, struct load_unload_state *);
static void readfile(const char *, struct load_unload_state *);
static int _fd(int);
static int demux_cmd(int argc, char *const argv[]);
static launch_data_t do_rendezvous_magic(const struct addrinfo *res, const char *serv);
static void submit_job_pass(launch_data_t jobs);
static void submit_mach_jobs(launch_data_t jobs);
static void let_go_of_mach_jobs(launch_data_t jobs);
static void do_mgroup_join(int fd, int family, int socktype, int protocol, const char *mgroup);
static mach_port_t str2bsport(const char *s);
static void print_jobs(launch_data_t j);
static void print_obj(launch_data_t obj, const char *key, void *context);
static bool is_legacy_mach_job(launch_data_t obj);
static bool delay_to_second_pass(launch_data_t o);
static void delay_to_second_pass2(launch_data_t o, const char *key, void *context);
static bool str2lim(const char *buf, rlim_t *res);
static const char *lim2str(rlim_t val, char *buf);
static const char *num2name(int n);
static ssize_t name2num(const char *n);
static void unloadjob(launch_data_t job);
static void print_key_value(launch_data_t obj, const char *key, void *context);
static void print_launchd_env(launch_data_t obj, const char *key, void *context);
static void _log_launchctl_bug(const char *rcs_rev, const char *path, unsigned int line, const char *test);
static void loopback_setup(void);
static pid_t fwexec(const char *const *argv, bool _wait);
static void do_potential_fsck(void);
static bool path_check(const char *path);
static bool is_safeboot(void);
static bool is_netboot(void);
static void apply_func_to_dir(const char *thedir, void (*thefunc)(const char *));
static void apply_sysctls_from_file(const char *thefile);
static void empty_dir(const char *path);
static int touch_file(const char *path, mode_t m);
static void do_sysversion_sysctl(void);
static void workaround4465949(void);

static int bootstrap_cmd(int argc, char *const argv[]);
static int load_and_unload_cmd(int argc, char *const argv[]);
//static int reload_cmd(int argc, char *const argv[]);
static int start_stop_remove_cmd(int argc, char *const argv[]);
static int submit_cmd(int argc, char *const argv[]);
static int list_cmd(int argc, char *const argv[]);

static int setenv_cmd(int argc, char *const argv[]);
static int unsetenv_cmd(int argc, char *const argv[]);
static int getenv_and_export_cmd(int argc, char *const argv[]);

static int limit_cmd(int argc, char *const argv[]);
static int stdio_cmd(int argc, char *const argv[]);
static int fyi_cmd(int argc, char *const argv[]);
static int logupdate_cmd(int argc, char *const argv[]);
static int umask_cmd(int argc, char *const argv[]);
static int getrusage_cmd(int argc, char *const argv[]);
static int bsexec_cmd(int argc, char *const argv[]);
static int bslist_cmd(int argc, char *const argv[]);

static int exit_cmd(int argc, char *const argv[]) __attribute__((noreturn));
static int help_cmd(int argc, char *const argv[]);

static const struct {
	const char *name;
	int (*func)(int argc, char *const argv[]);
	const char *desc;
} cmds[] = {
	{ "load",	load_and_unload_cmd,	"Load configuration files and/or directories" },
	{ "unload",	load_and_unload_cmd,	"Unload configuration files and/or directories" },
//	{ "reload",	reload_cmd,		"Reload configuration files and/or directories" },
	{ "start",	start_stop_remove_cmd,	"Start specified job" },
	{ "stop",	start_stop_remove_cmd,	"Stop specified job" },
	{ "submit",	submit_cmd,		"Submit a job from the command line" },
	{ "remove",	start_stop_remove_cmd,	"Remove specified job" },
	{ "bootstrap",	bootstrap_cmd,		"Bootstrap launchd" },
	{ "list",	list_cmd,		"List jobs and information about jobs" },
	{ "setenv",	setenv_cmd,		"Set an environmental variable in launchd" },
	{ "unsetenv",	unsetenv_cmd,		"Unset an environmental variable in launchd" },
	{ "getenv",	getenv_and_export_cmd,	"Get an environmental variable from launchd" },
	{ "export",	getenv_and_export_cmd,	"Export shell settings from launchd" },
	{ "limit",	limit_cmd,		"View and adjust launchd resource limits" },
	{ "stdout",	stdio_cmd,		"Redirect launchd's standard out to the given path" },
	{ "stderr",	stdio_cmd,		"Redirect launchd's standard error to the given path" },
	{ "shutdown",	fyi_cmd,		"Prepare for system shutdown" },
	{ "singleuser",	fyi_cmd,		"Switch to single-user mode" },
	{ "reloadttys",	fyi_cmd,		"Reload /etc/ttys" },
	{ "getrusage",	getrusage_cmd,		"Get resource usage statistics from launchd" },
	{ "log",	logupdate_cmd,		"Adjust the logging level or mask of launchd" },
	{ "umask",	umask_cmd,		"Change launchd's umask" },
	{ "bsexec",	bsexec_cmd,		"Execute a process within a different Mach bootstrap subset" },
	{ "bslist",	bslist_cmd,		"List Mach bootstrap services and optional servers" },
	{ "exit",	exit_cmd,		"Exit the interactive invocation of launchctl" },
	{ "quit",	exit_cmd,		"Quit the interactive invocation of launchctl" },
	{ "help",	help_cmd,		"This help output" },
};

static bool istty = false;
static bool verbose = false;

int
main(int argc, char *const argv[])
{
	char *l;

	do_sysversion_sysctl();

	istty = isatty(STDIN_FILENO);

	argc--, argv++;

	if (argc > 0 && argv[0][0] == '-') {
		char *flago;

		for (flago = argv[0] + 1; *flago; flago++) {
			switch (*flago) {
			case 'v':
				verbose = true;
				break;
			default:
				fprintf(stderr, "Unknown argument: '-%c'\n", *flago);
				break;
			}
		}
		argc--, argv++;
	}

	if (NULL == readline) {
		fprintf(stderr, "missing library: readline\n");
		exit(EXIT_FAILURE);
	}

	if (argc == 0) {
		while ((l = readline(istty ? "launchd% " : NULL))) {
			char *inputstring = l, *argv2[100], **ap = argv2;
			int i = 0;

			while ((*ap = strsep(&inputstring, " \t"))) {
				if (**ap != '\0') {
					ap++;
					i++;
				}
			}

			if (i > 0)
				demux_cmd(i, argv2);

			free(l);
		}

		if (istty) {
			fputc('\n', stdout);
		}
	}

	if (argc > 0) {
		exit(demux_cmd(argc, argv));
	}

	exit(EXIT_SUCCESS);
}

int
demux_cmd(int argc, char *const argv[])
{
	size_t i;

	optind = 1;
	optreset = 1;

	for (i = 0; i < (sizeof cmds / sizeof cmds[0]); i++) {
		if (!strcmp(cmds[i].name, argv[0]))
			return cmds[i].func(argc, argv);
	}

	fprintf(stderr, "%s: unknown subcommand \"%s\"\n", getprogname(), argv[0]);
	return 1;
}

int
unsetenv_cmd(int argc, char *const argv[])
{
	launch_data_t resp, tmp, msg;

	if (argc != 2) {
		fprintf(stderr, "%s usage: unsetenv <key>\n", getprogname());
		return 1;
	}

	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);

	tmp = launch_data_new_string(argv[1]);
	launch_data_dict_insert(msg, tmp, LAUNCH_KEY_UNSETUSERENVIRONMENT);

	resp = launch_msg(msg);

	launch_data_free(msg);

	if (resp) {
		launch_data_free(resp);
	} else {
		fprintf(stderr, "launch_msg(\"%s\"): %s\n", LAUNCH_KEY_UNSETUSERENVIRONMENT, strerror(errno));
	}

	return 0;
}

int
setenv_cmd(int argc, char *const argv[])
{
	launch_data_t resp, tmp, tmpv, msg;

	if (argc != 3) {
		fprintf(stderr, "%s usage: setenv <key> <value>\n", getprogname());
		return 1;
	}

	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	tmp = launch_data_alloc(LAUNCH_DATA_DICTIONARY);

	tmpv = launch_data_new_string(argv[2]);
	launch_data_dict_insert(tmp, tmpv, argv[1]);
	launch_data_dict_insert(msg, tmp, LAUNCH_KEY_SETUSERENVIRONMENT);

	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp) {
		launch_data_free(resp);
	} else {
		fprintf(stderr, "launch_msg(\"%s\"): %s\n", LAUNCH_KEY_SETUSERENVIRONMENT, strerror(errno));
	}

	return 0;
}

void
print_launchd_env(launch_data_t obj, const char *key, void *context)
{
	bool *is_csh = context;

	/* XXX escape the double quotes */
	if (*is_csh)
		fprintf(stdout, "setenv %s \"%s\";\n", key, launch_data_get_string(obj));
	else
		fprintf(stdout, "%s=\"%s\"; export %s;\n", key, launch_data_get_string(obj), key);
}

void
print_key_value(launch_data_t obj, const char *key, void *context)
{
	const char *k = context;

	if (!strcmp(key, k))
		fprintf(stdout, "%s\n", launch_data_get_string(obj));
}

int
getenv_and_export_cmd(int argc, char *const argv[] __attribute__((unused)))
{
	launch_data_t resp, msg;
	bool is_csh = false;
	char *k;
	
	if (!strcmp(argv[0], "export")) {
		char *s = getenv("SHELL");
		if (s)
			is_csh = strstr(s, "csh") ? true : false;
	} else if (argc != 2) {
		fprintf(stderr, "%s usage: getenv <key>\n", getprogname());
		return 1;
	}

	k = argv[1];

	msg = launch_data_new_string(LAUNCH_KEY_GETUSERENVIRONMENT);

	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp) {
		if (!strcmp(argv[0], "export"))
			launch_data_dict_iterate(resp, print_launchd_env, &is_csh);
		else
			launch_data_dict_iterate(resp, print_key_value, k);
		launch_data_free(resp);
	} else {
		fprintf(stderr, "launch_msg(\"" LAUNCH_KEY_GETUSERENVIRONMENT "\"): %s\n", strerror(errno));
	}
	return 0;
}

void
unloadjob(launch_data_t job)
{
	launch_data_t resp, tmp, tmps, msg;
	int e;

	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	tmp = launch_data_alloc(LAUNCH_DATA_STRING);
	tmps = launch_data_dict_lookup(job, LAUNCH_JOBKEY_LABEL);

	if (!tmps) {
		fprintf(stderr, "%s: Error: Missing Key: %s\n", getprogname(), LAUNCH_JOBKEY_LABEL);
		return;
	}

	launch_data_set_string(tmp, launch_data_get_string(tmps));
	launch_data_dict_insert(msg, tmp, LAUNCH_KEY_REMOVEJOB);
	resp = launch_msg(msg);
	launch_data_free(msg);
	if (!resp) {
		fprintf(stderr, "%s: Error: launch_msg(): %s\n", getprogname(), strerror(errno));
		return;
	}
	if (LAUNCH_DATA_ERRNO == launch_data_get_type(resp)) {
		if ((e = launch_data_get_errno(resp)))
			fprintf(stderr, "%s\n", strerror(e));
	}
	launch_data_free(resp);
}

launch_data_t
read_plist_file(const char *file, bool editondisk, bool load)
{
	CFPropertyListRef plist = CreateMyPropertyListFromFile(file);
	launch_data_t r = NULL;

	if (NULL == plist) {
		fprintf(stderr, "%s: no plist was returned for: %s\n", getprogname(), file);
		return NULL;
	}

	if (editondisk) {
		if (load)
			CFDictionaryRemoveValue((CFMutableDictionaryRef)plist, CFSTR(LAUNCH_JOBKEY_DISABLED));
		else
			CFDictionarySetValue((CFMutableDictionaryRef)plist, CFSTR(LAUNCH_JOBKEY_DISABLED), kCFBooleanTrue);
		WriteMyPropertyListToFile(plist, file);
	}

	r = CF2launch_data(plist);

	CFRelease(plist);

	return r;
}

void
delay_to_second_pass2(launch_data_t o, const char *key, void *context)
{
	bool *res = context;
	size_t i;

	if (key && 0 == strcmp(key, LAUNCH_JOBSOCKETKEY_BONJOUR)) {
		*res = true;
		return;
	}

	switch (launch_data_get_type(o)) {
	case LAUNCH_DATA_DICTIONARY:
		launch_data_dict_iterate(o, delay_to_second_pass2, context);
		break;
	case LAUNCH_DATA_ARRAY:
		for (i = 0; i < launch_data_array_get_count(o); i++)
			delay_to_second_pass2(launch_data_array_get_index(o, i), NULL, context);
		break;
	default:
		break;
	}
}

bool
delay_to_second_pass(launch_data_t o)
{
	bool res = false;

	launch_data_t socks = launch_data_dict_lookup(o, LAUNCH_JOBKEY_SOCKETS);

	if (NULL == socks)
		return false;

	delay_to_second_pass2(socks, NULL, &res);

	return res;
}

void
readfile(const char *what, struct load_unload_state *lus)
{
	char ourhostname[1024];
	launch_data_t tmpd, tmps, thejob, tmpa;
	bool job_disabled = false;
	size_t i, c;

	gethostname(ourhostname, sizeof(ourhostname));

	if (NULL == (thejob = read_plist_file(what, lus->editondisk, lus->load))) {
		fprintf(stderr, "%s: no plist was returned for: %s\n", getprogname(), what);
		return;
	}

	if (is_legacy_mach_job(thejob)) {
		fprintf(stderr, "%s: Please convert the following to launchd: %s\n", getprogname(), what);
		launch_data_array_append(lus->pass0, thejob);
		return;
	}

	if (NULL == launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_LABEL)) {
		fprintf(stderr, "%s: missing the Label key: %s\n", getprogname(), what);
		goto out_bad;
	}

	if (NULL != (tmpa = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_LIMITLOADFROMHOSTS))) {
		c = launch_data_array_get_count(tmpa);

		for (i = 0; i < c; i++) {
			launch_data_t oai = launch_data_array_get_index(tmpa, i);
			if (!strcasecmp(ourhostname, launch_data_get_string(oai)))
				goto out_bad;
		}
	}

	if (NULL != (tmpa = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_LIMITLOADTOHOSTS))) {
		c = launch_data_array_get_count(tmpa);

		for (i = 0; i < c; i++) {
			launch_data_t oai = launch_data_array_get_index(tmpa, i);
			if (!strcasecmp(ourhostname, launch_data_get_string(oai)))
				break;
		}

		if (i == c)
			goto out_bad;
	}

	if ((tmpa = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_LIMITLOADTOSESSIONTYPE))) {
		const char *allowed_session;
		bool skipjob = true;

		if (lus->session_type) switch (launch_data_get_type(tmpa)) {
		case LAUNCH_DATA_ARRAY:
			c = launch_data_array_get_count(tmpa);
			for (i = 0; i < c; i++) {
				tmps = launch_data_array_get_index(tmpa, i);
				allowed_session = launch_data_get_string(tmps);
				if (strcasecmp(lus->session_type, allowed_session) == 0) {
					skipjob = false;
					break;
				}
			}
			break;
		case LAUNCH_DATA_STRING:
			allowed_session = launch_data_get_string(tmpa);
			if (strcasecmp(lus->session_type, allowed_session) == 0) {
				skipjob = false;
			}
			break;
		default:
			break;
		}

		if (skipjob) {
			goto out_bad;
		}
	}

	if ((tmpd = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_DISABLED)))
		job_disabled = launch_data_get_bool(tmpd);

	if (lus->forceload)
		job_disabled = false;

	if (job_disabled && lus->load)
		goto out_bad;

	if (delay_to_second_pass(thejob))
		launch_data_array_append(lus->pass2, thejob);
	else
		launch_data_array_append(lus->pass1, thejob);

	if (verbose)
		fprintf(stdout, "Will load: %s\n", what);

	return;
out_bad:
	if (verbose)
		fprintf(stdout, "Ignored: %s\n", what);
	launch_data_free(thejob);
}

void
readpath(const char *what, struct load_unload_state *lus)
{
	char buf[MAXPATHLEN];
	struct stat sb;
	struct dirent *de;
	DIR *d;

	if (stat(what, &sb) == -1)
		return;

	if (S_ISREG(sb.st_mode) && !(sb.st_mode & S_IWOTH)) {
		readfile(what, lus);
	} else {
		if ((d = opendir(what)) == NULL) {
			fprintf(stderr, "%s: opendir() failed to open the directory\n", getprogname());
			return;
		}

		while ((de = readdir(d))) {
			if ((de->d_name[0] == '.'))
				continue;
			snprintf(buf, sizeof(buf), "%s/%s", what, de->d_name);

			readfile(buf, lus);
		}
		closedir(d);
	}
}

struct distill_context {
	launch_data_t base;
	launch_data_t newsockdict;
};

void
distill_jobs(launch_data_t jobs)
{
	size_t i, c = launch_data_array_get_count(jobs);

	for (i = 0; i < c; i++)
		distill_config_file(launch_data_array_get_index(jobs, i));
}

void
distill_config_file(launch_data_t id_plist)
{
	struct distill_context dc = { id_plist, NULL };
	launch_data_t tmp;

	if ((tmp = launch_data_dict_lookup(dc.base, LAUNCH_JOBKEY_SOCKETS))) {
		dc.newsockdict = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
		launch_data_dict_iterate(tmp, sock_dict_cb, &dc);
		launch_data_dict_insert(dc.base, dc.newsockdict, LAUNCH_JOBKEY_SOCKETS);
	}
}

void
sock_dict_cb(launch_data_t what, const char *key, void *context)
{
	struct distill_context *dc = context;
	launch_data_t fdarray = launch_data_alloc(LAUNCH_DATA_ARRAY);

	launch_data_dict_insert(dc->newsockdict, fdarray, key);

	if (launch_data_get_type(what) == LAUNCH_DATA_DICTIONARY) {
		sock_dict_edit_entry(what, key, fdarray, dc->base);
	} else if (launch_data_get_type(what) == LAUNCH_DATA_ARRAY) {
		launch_data_t tmp;
		size_t i;

		for (i = 0; i < launch_data_array_get_count(what); i++) {
			tmp = launch_data_array_get_index(what, i);
			sock_dict_edit_entry(tmp, key, fdarray, dc->base);
		}
	}
}

void
sock_dict_edit_entry(launch_data_t tmp, const char *key, launch_data_t fdarray, launch_data_t thejob)
{
	launch_data_t a, val;
	int sfd, st = SOCK_STREAM;
	bool passive = true;

	if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_TYPE))) {
		if (!strcasecmp(launch_data_get_string(val), "stream")) {
			st = SOCK_STREAM;
		} else if (!strcasecmp(launch_data_get_string(val), "dgram")) {
			st = SOCK_DGRAM;
		} else if (!strcasecmp(launch_data_get_string(val), "seqpacket")) {
			st = SOCK_SEQPACKET;
		}
	}

	if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_PASSIVE)))
		passive = launch_data_get_bool(val);

	if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_SECUREWITHKEY))) {
		char secdir[] = LAUNCH_SECDIR, buf[1024];
		launch_data_t uenv = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_USERENVIRONMENTVARIABLES);

		if (NULL == uenv) {
			uenv = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
			launch_data_dict_insert(thejob, uenv, LAUNCH_JOBKEY_USERENVIRONMENTVARIABLES);
		}

		mkdtemp(secdir);

		sprintf(buf, "%s/%s", secdir, key);

		a = launch_data_new_string(buf);
		launch_data_dict_insert(tmp, a, LAUNCH_JOBSOCKETKEY_PATHNAME);
		a = launch_data_new_string(buf);
		launch_data_dict_insert(uenv, a, launch_data_get_string(val));
	}
		
	if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_PATHNAME))) {
		struct sockaddr_un sun;
		mode_t sun_mode = 0;
		mode_t oldmask;
		bool setm = false;

		memset(&sun, 0, sizeof(sun));

		sun.sun_family = AF_UNIX;

		strncpy(sun.sun_path, launch_data_get_string(val), sizeof(sun.sun_path));
	
		if ((sfd = _fd(socket(AF_UNIX, st, 0))) == -1)
			return;

		if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_PATHMODE))) {
			sun_mode = (mode_t)launch_data_get_integer(val);
			setm = true;
		}

		if (passive) {                  
			if (unlink(sun.sun_path) == -1 && errno != ENOENT) {
				close(sfd);     
				return;
			}
			oldmask = umask(S_IRWXG|S_IRWXO);
			if (bind(sfd, (struct sockaddr *)&sun, sizeof(sun)) == -1) {
				close(sfd);
				umask(oldmask);
				return;
			}
			umask(oldmask);
			if (setm) {
				chmod(sun.sun_path, sun_mode);
			}
			if ((st == SOCK_STREAM || st == SOCK_SEQPACKET)
					&& listen(sfd, SOMAXCONN) == -1) {
				close(sfd);
				return;
			}
		} else if (connect(sfd, (struct sockaddr *)&sun, sizeof(sun)) == -1) {
			close(sfd);
			return;
		}

		val = launch_data_new_fd(sfd);
		launch_data_array_append(fdarray, val);
	} else {
		launch_data_t rnames = NULL;
		const char *node = NULL, *serv = NULL, *mgroup = NULL;
		char servnbuf[50];
		struct addrinfo hints, *res0, *res;
		int gerr, sock_opt = 1;
		bool rendezvous = false;

		memset(&hints, 0, sizeof(hints));

		hints.ai_socktype = st;
		if (passive)
			hints.ai_flags |= AI_PASSIVE;

		if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_NODENAME)))
			node = launch_data_get_string(val);
		if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_MULTICASTGROUP)))
			mgroup = launch_data_get_string(val);
		if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_SERVICENAME))) {
			if (LAUNCH_DATA_INTEGER == launch_data_get_type(val)) {
				sprintf(servnbuf, "%lld", launch_data_get_integer(val));
				serv = servnbuf;
			} else {
				serv = launch_data_get_string(val);
			}
		}
		if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_FAMILY))) {
			if (!strcasecmp("IPv4", launch_data_get_string(val)))
				hints.ai_family = AF_INET;
			else if (!strcasecmp("IPv6", launch_data_get_string(val)))
				hints.ai_family = AF_INET6;
		}
		if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_PROTOCOL))) {
			if (!strcasecmp("TCP", launch_data_get_string(val)))
				hints.ai_protocol = IPPROTO_TCP;
		}
		if ((rnames = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_BONJOUR))) {
			rendezvous = true;
			if (LAUNCH_DATA_BOOL == launch_data_get_type(rnames)) {
				rendezvous = launch_data_get_bool(rnames);
				rnames = NULL;
			}
		}

		if ((gerr = getaddrinfo(node, serv, &hints, &res0)) != 0) {
			fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(gerr));
			return;
		}

		for (res = res0; res; res = res->ai_next) {
			launch_data_t rvs_fd = NULL;
			if ((sfd = _fd(socket(res->ai_family, res->ai_socktype, res->ai_protocol))) == -1) {
				fprintf(stderr, "socket(): %s\n", strerror(errno));
				return;
			}
			if (hints.ai_flags & AI_PASSIVE) {
				if (AF_INET6 == res->ai_family && -1 == setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY,
							(void *)&sock_opt, sizeof(sock_opt))) {
					fprintf(stderr, "setsockopt(IPV6_V6ONLY): %m");
					return;
				}
				if (mgroup) {
					if (setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, (void *)&sock_opt, sizeof(sock_opt)) == -1) {
						fprintf(stderr, "setsockopt(SO_REUSEPORT): %s\n", strerror(errno));
						return;
					}
				} else {
					if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&sock_opt, sizeof(sock_opt)) == -1) {
						fprintf(stderr, "setsockopt(SO_REUSEADDR): %s\n", strerror(errno));
						return;
					}
				}
				if (bind(sfd, res->ai_addr, res->ai_addrlen) == -1) {
					fprintf(stderr, "bind(): %s\n", strerror(errno));
					return;
				}

				if (mgroup) {
					do_mgroup_join(sfd, res->ai_family, res->ai_socktype, res->ai_protocol, mgroup);
				}
				if ((res->ai_socktype == SOCK_STREAM || res->ai_socktype == SOCK_SEQPACKET)
						&& listen(sfd, SOMAXCONN) == -1) {
					fprintf(stderr, "listen(): %s\n", strerror(errno));
					return;
				}
				if (rendezvous && (res->ai_family == AF_INET || res->ai_family == AF_INET6) &&
						(res->ai_socktype == SOCK_STREAM || res->ai_socktype == SOCK_DGRAM)) {
					launch_data_t rvs_fds = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_BONJOURFDS);
					if (NULL == rvs_fds) {
						rvs_fds = launch_data_alloc(LAUNCH_DATA_ARRAY);
						launch_data_dict_insert(thejob, rvs_fds, LAUNCH_JOBKEY_BONJOURFDS);
					}
					if (NULL == rnames) {
						rvs_fd = do_rendezvous_magic(res, serv);
						if (rvs_fd)
							launch_data_array_append(rvs_fds, rvs_fd);
					} else if (LAUNCH_DATA_STRING == launch_data_get_type(rnames)) {
						rvs_fd = do_rendezvous_magic(res, launch_data_get_string(rnames));
						if (rvs_fd)
							launch_data_array_append(rvs_fds, rvs_fd);
					} else if (LAUNCH_DATA_ARRAY == launch_data_get_type(rnames)) {
						size_t rn_i, rn_ac = launch_data_array_get_count(rnames);

						for (rn_i = 0; rn_i < rn_ac; rn_i++) {
							launch_data_t rn_tmp = launch_data_array_get_index(rnames, rn_i);

							rvs_fd = do_rendezvous_magic(res, launch_data_get_string(rn_tmp));
							if (rvs_fd)
								launch_data_array_append(rvs_fds, rvs_fd);
						}
					}
				}
			} else {
				if (connect(sfd, res->ai_addr, res->ai_addrlen) == -1) {
					fprintf(stderr, "connect(): %s\n", strerror(errno));
					return;
				}
			}
			val = launch_data_new_fd(sfd);
			if (rvs_fd) {
				/* <rdar://problem/3964648> Launchd should not register the same service more than once */
				/* <rdar://problem/3965154> Switch to DNSServiceRegisterAddrInfo() */
				rendezvous = false;
			}
			launch_data_array_append(fdarray, val);
		}
	}
}

void
do_mgroup_join(int fd, int family, int socktype, int protocol, const char *mgroup)
{
	struct addrinfo hints, *res0, *res;
	struct ip_mreq mreq;
	struct ipv6_mreq m6req;
	int gerr;

	memset(&hints, 0, sizeof(hints));

	hints.ai_flags |= AI_PASSIVE;
	hints.ai_family = family;
	hints.ai_socktype = socktype;
	hints.ai_protocol = protocol;

	if ((gerr = getaddrinfo(mgroup, NULL, &hints, &res0)) != 0) {
		fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(gerr));
		return;
	}

	for (res = res0; res; res = res->ai_next) {
		if (AF_INET == family) {
			memset(&mreq, 0, sizeof(mreq));
			mreq.imr_multiaddr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
			if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == -1) {
				fprintf(stderr, "setsockopt(IP_ADD_MEMBERSHIP): %s\n", strerror(errno));
				continue;
			}
			break;
		} else if (AF_INET6 == family) {
			memset(&m6req, 0, sizeof(m6req));
			m6req.ipv6mr_multiaddr = ((struct sockaddr_in6 *)res->ai_addr)->sin6_addr;
			if (setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &m6req, sizeof(m6req)) == -1) {
				fprintf(stderr, "setsockopt(IPV6_JOIN_GROUP): %s\n", strerror(errno));
				continue;
			}
			break;
		} else {
			fprintf(stderr, "unknown family during multicast group bind!\n");
			break;
		}
	}

	freeaddrinfo(res0);
}


launch_data_t
do_rendezvous_magic(const struct addrinfo *res, const char *serv)
{
	struct stat sb;
	DNSServiceRef service;
	DNSServiceErrorType error;
	char rvs_buf[200];
	short port;
	static int statres = 1;

	if (1 == statres)
		statres = stat("/usr/sbin/mDNSResponder", &sb);

	if (-1 == statres)
		return NULL;

	sprintf(rvs_buf, "_%s._%s.", serv, res->ai_socktype == SOCK_STREAM ? "tcp" : "udp");

	if (res->ai_family == AF_INET)
		port = ((struct sockaddr_in *)res->ai_addr)->sin_port;
	else
		port = ((struct sockaddr_in6 *)res->ai_addr)->sin6_port;

	error = DNSServiceRegister(&service, 0, 0, NULL, rvs_buf, NULL, NULL, port, 0, NULL, NULL, NULL);

	if (error == kDNSServiceErr_NoError)
		return launch_data_new_fd(DNSServiceRefSockFD(service));

	fprintf(stderr, "DNSServiceRegister(\"%s\"): %d\n", serv, error);
	return NULL;
}

CFPropertyListRef
CreateMyPropertyListFromFile(const char *posixfile)
{
	CFPropertyListRef propertyList;
	CFStringRef       errorString;
	CFDataRef         resourceData;
	SInt32            errorCode;
	CFURLRef          fileURL;

	fileURL = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)posixfile, strlen(posixfile), false);
	if (!fileURL)
		fprintf(stderr, "%s: CFURLCreateFromFileSystemRepresentation(%s) failed\n", getprogname(), posixfile);
	if (!CFURLCreateDataAndPropertiesFromResource(kCFAllocatorDefault, fileURL, &resourceData, NULL, NULL, &errorCode))
		fprintf(stderr, "%s: CFURLCreateDataAndPropertiesFromResource(%s) failed: %d\n", getprogname(), posixfile, (int)errorCode);
	propertyList = CFPropertyListCreateFromXMLData(kCFAllocatorDefault, resourceData, kCFPropertyListMutableContainers, &errorString);
	if (!propertyList)
		fprintf(stderr, "%s: propertyList is NULL\n", getprogname());

	return propertyList;
}

void
WriteMyPropertyListToFile(CFPropertyListRef plist, const char *posixfile)
{
	CFDataRef	resourceData;
	CFURLRef	fileURL;
	SInt32		errorCode;

	fileURL = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)posixfile, strlen(posixfile), false);
	if (!fileURL)
		fprintf(stderr, "%s: CFURLCreateFromFileSystemRepresentation(%s) failed\n", getprogname(), posixfile);
	resourceData = CFPropertyListCreateXMLData(kCFAllocatorDefault, plist);
	if (resourceData == NULL)
		fprintf(stderr, "%s: CFPropertyListCreateXMLData(%s) failed", getprogname(), posixfile);
	if (!CFURLWriteDataAndPropertiesToResource(fileURL, resourceData, NULL, &errorCode))
		fprintf(stderr, "%s: CFURLWriteDataAndPropertiesToResource(%s) failed: %d\n", getprogname(), posixfile, (int)errorCode);
}

void
myCFDictionaryApplyFunction(const void *key, const void *value, void *context)
{
	launch_data_t ik, iw, where = context;

	ik = CF2launch_data(key);
	iw = CF2launch_data(value);

	launch_data_dict_insert(where, iw, launch_data_get_string(ik));
	launch_data_free(ik);
}

launch_data_t
CF2launch_data(CFTypeRef cfr)
{
	launch_data_t r;
	CFTypeID cft = CFGetTypeID(cfr);

	if (cft == CFStringGetTypeID()) {
		char buf[4096];
		CFStringGetCString(cfr, buf, sizeof(buf), kCFStringEncodingUTF8);
		r = launch_data_alloc(LAUNCH_DATA_STRING);
		launch_data_set_string(r, buf);
	} else if (cft == CFBooleanGetTypeID()) {
		r = launch_data_alloc(LAUNCH_DATA_BOOL);
		launch_data_set_bool(r, CFBooleanGetValue(cfr));
	} else if (cft == CFArrayGetTypeID()) {
		CFIndex i, ac = CFArrayGetCount(cfr);
		r = launch_data_alloc(LAUNCH_DATA_ARRAY);
		for (i = 0; i < ac; i++) {
			CFTypeRef v = CFArrayGetValueAtIndex(cfr, i);
			if (v) {
				launch_data_t iv = CF2launch_data(v);
				launch_data_array_set_index(r, iv, i);
			}
		}
	} else if (cft == CFDictionaryGetTypeID()) {
		r = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
		CFDictionaryApplyFunction(cfr, myCFDictionaryApplyFunction, r);
	} else if (cft == CFDataGetTypeID()) {
		r = launch_data_alloc(LAUNCH_DATA_ARRAY);
		launch_data_set_opaque(r, CFDataGetBytePtr(cfr), CFDataGetLength(cfr));
	} else if (cft == CFNumberGetTypeID()) {
		long long n;
		double d;
		CFNumberType cfnt = CFNumberGetType(cfr);
		switch (cfnt) {
		case kCFNumberSInt8Type:
		case kCFNumberSInt16Type:
		case kCFNumberSInt32Type:
		case kCFNumberSInt64Type:
		case kCFNumberCharType:
		case kCFNumberShortType:
		case kCFNumberIntType:
		case kCFNumberLongType:
		case kCFNumberLongLongType:
			CFNumberGetValue(cfr, kCFNumberLongLongType, &n);
			r = launch_data_alloc(LAUNCH_DATA_INTEGER);
			launch_data_set_integer(r, n);
			break;
		case kCFNumberFloat32Type:
		case kCFNumberFloat64Type:
		case kCFNumberFloatType:
		case kCFNumberDoubleType:
			CFNumberGetValue(cfr, kCFNumberDoubleType, &d);
			r = launch_data_alloc(LAUNCH_DATA_REAL);
			launch_data_set_real(r, d);
			break;
		default:
			r = NULL;
			break;
		}
	} else {
		r = NULL;
	}
	return r;
}

int
help_cmd(int argc, char *const argv[])
{
	FILE *where = stdout;
	int l, cmdwidth = 0;
	size_t i;
	
	if (argc == 0 || argv == NULL)
		where = stderr;

	fprintf(where, "usage: %s <subcommand>\n", getprogname());

	for (i = 0; i < (sizeof cmds / sizeof cmds[0]); i++) {
		l = strlen(cmds[i].name);
		if (l > cmdwidth)
			cmdwidth = l;
	}

	for (i = 0; i < (sizeof cmds / sizeof cmds[0]); i++)
		fprintf(where, "\t%-*s\t%s\n", cmdwidth, cmds[i].name, cmds[i].desc);

	return 0;
}

int
exit_cmd(int argc __attribute__((unused)), char *const argv[] __attribute__((unused)))
{
	exit(0);
}

int
_fd(int fd)
{
	if (fd >= 0)
		fcntl(fd, F_SETFD, 1);
	return fd;
}

int
bootstrap_cmd(int argc __attribute__((unused)), char *const argv[] __attribute__((unused)))
{
	int memmib[] = { CTL_HW, HW_MEMSIZE };
	int mvnmib[] = { CTL_KERN, KERN_MAXVNODES };
	int hnmib[] = { CTL_KERN, KERN_HOSTNAME };
	uint64_t mem = 0;
	uint32_t mvn;
	size_t memsz = sizeof(mem);
	struct group *tfp_gr;

	if (assumes((tfp_gr = getgrnam("procview")) != NULL)) {
		int tfp_r_mib[3] = { CTL_KERN, KERN_TFP, KERN_TFP_READ_GROUP };
		gid_t tfp_r_gid = tfp_gr->gr_gid;
		assumes(sysctl(tfp_r_mib, 3, NULL, NULL, &tfp_r_gid, sizeof(tfp_r_gid)) != -1);
	}

	if (assumes((tfp_gr = getgrnam("procmod")) != NULL)) {
		int tfp_rw_mib[3] = { CTL_KERN, KERN_TFP, KERN_TFP_RW_GROUP };
		gid_t tfp_rw_gid = tfp_gr->gr_gid;
		assumes(sysctl(tfp_rw_mib, 3, NULL, NULL, &tfp_rw_gid, sizeof(tfp_rw_gid)) != -1);
	}

        if (assumes(sysctl(memmib, 2, &mem, &memsz, NULL, 0) != -1)) {
		mvn = mem / (64 * 1024) + 1024;
		assumes(sysctl(mvnmib, 2, NULL, NULL, &mvn, sizeof(mvn)) != -1);
	}
	assumes(sysctl(hnmib, 2, NULL, NULL, "localhost", sizeof("localhost")) != -1);

	loopback_setup();

	apply_sysctls_from_file("/etc/sysctl-macosxserver.conf");
	apply_sysctls_from_file("/etc/sysctl.conf");

	if (path_check("/System/Installation") && path_check("/etc/rc.cdrom")) {
		const char *rccdrom_tool[] = { _PATH_BSHELL, "/etc/rc.cdrom", "multiuser", NULL };
		assumes(fwexec(rccdrom_tool, true) != -1);
		assumes(reboot(RB_HALT) != -1);
		_exit(EXIT_FAILURE);
	} else if (is_netboot()) {
		const char *rcnetboot_tool[] = { _PATH_BSHELL, "/etc/rc.netboot", "init", NULL };
		if (!assumes(fwexec(rcnetboot_tool, true) != -1)) {
			assumes(reboot(RB_HALT) != -1);
			_exit(EXIT_FAILURE);
		}
	} else {
		do_potential_fsck();
	}

	if (path_check("/var/account/acct")) {
		assumes(acct("/var/account/acct") != -1);
	}

	if (path_check("/etc/fstab")) {
		const char *mount_tool[] = { "mount", "-vat", "nonfs", NULL };
		assumes(fwexec(mount_tool, true) != -1);
	}

	if (path_check("/etc/rc.installer_cleanup")) {
		const char *rccleanup_tool[] = { _PATH_BSHELL, "/etc/rc.installer_cleanup", "multiuser", NULL };
		assumes(fwexec(rccleanup_tool, true) != -1);
	}

	apply_func_to_dir(_PATH_VARRUN, empty_dir);
	apply_func_to_dir(_PATH_TMP, empty_dir);
	remove(_PATH_NOLOGIN);

	// XXX --> RMRF_ITEMS="/var/tmp/folders.*

	// 775 www:www /var/run/davlocks 4489695
	// 775 root:daemon /var/run/StartupItems

	assumes(touch_file(_PATH_UTMP, DEFFILEMODE) != -1);
	assumes(touch_file(_PATH_UTMPX, DEFFILEMODE) != -1);
	assumes(touch_file(_PATH_VARRUN "/.systemStarterRunning", DEFFILEMODE) != -1);

	if (!path_check("/var/db/netinfo/local.nidb")) {
		const char *create_nidb_tool[] = { "/usr/libexec/create_nidb", NULL };

		fprintf(stderr, "NetInfo database missing. Creating...");

		mkdir("/var/db/netinfo", ACCESSPERMS);
		remove("/var/db/.AppleSetupDone");
		assumes(fwexec(create_nidb_tool, true) != -1);
	}

	if (path_check("/etc/security/rc.audit")) {
		const char *audit_tool[] = { _PATH_BSHELL, "/etc/security/rc.audit", NULL };
		assumes(fwexec(audit_tool, true) != -1);
	}

	if (path_check("/Library/Preferences/com.apple.sharing.firewall.plist")) {
		const char *fw_tool[] = { "/usr/libexec/FirewallTool", NULL };
		assumes(fwexec(fw_tool, true) != -1);
	}

	const char *bcc_tool[] = { "BootCacheControl", "start", NULL };
	assumes(fwexec(bcc_tool, true) != -1);

	char *load_launchd_items[] = { "load", "-D", "all", "/etc/mach_init.d", NULL };
	if (is_safeboot())
		load_launchd_items[2] = "system";
	assumes(load_and_unload_cmd(4, load_launchd_items) == 0);

	const char *bcc_tag_tool[] = { "BootCacheControl", "tag", NULL };
	assumes(fwexec(bcc_tag_tool, true) != -1);

	const char *SystemStarter_tool[] = { "SystemStarter", NULL };
	assumes(fwexec(SystemStarter_tool, false) != -1);

	workaround4465949();

	if (path_check("/etc/rc.local")) {
		const char *rc_local_tool[] = { _PATH_BSHELL, "/etc/rc.local", NULL };
		assumes(fwexec(rc_local_tool, false) != -1);
	}

	return 0;
}

void
workaround4465949(void)
{
	const char *pbs_tool[] = { "/System/Library/CoreServices/pbs", NULL };
	const char *lca_tool[] = { "/System/Library/CoreServices/Language Chooser.app/Contents/MacOS/Language Chooser", NULL};
	char *const reloadttys_argv[] = { "reloadttys", NULL };
	int wstatus;
	pid_t pbs_p;

	if (path_check("/System/Library/LaunchDaemons/com.apple.loginwindow.plist")) {
		return;
	}

	if (path_check(pbs_tool[0]) && path_check(lca_tool[0]) &&
			!path_check("/var/db/.AppleSetupDone") &&
			path_check("/var/db/.RunLanguageChooserToo")) {
		if (assumes((pbs_p = fwexec(pbs_tool, false)) != -1)) {
			assumes(fwexec(lca_tool, true) != -1);
			assumes(kill(pbs_p, SIGTERM) != -1);
			assumes(waitpid(pbs_p, &wstatus, 0) != -1);
		}
	}

	assumes(fyi_cmd(1, reloadttys_argv) == 0);
}

int
load_and_unload_cmd(int argc, char *const argv[])
{
        NSSearchPathEnumerationState es = 0;
	char nspath[PATH_MAX * 2]; /* safe side, we need to append */
	bool badopts = false;
	struct load_unload_state lus;
	size_t i;
	int ch;

	memset(&lus, 0, sizeof(lus));

	if (!strcmp(argv[0], "load"))
		lus.load = true;

	while ((ch = getopt(argc, argv, "wFS:D:")) != -1) {
		switch (ch) {
		case 'w':
			lus.editondisk = true;
			break;
		case 'F':
			lus.forceload = true;
			break;
		case 'S':
			lus.session_type = optarg;
			break;
		case 'D':
			  if (strcasecmp(optarg, "all") == 0) {
				  es |= NSAllDomainsMask;
			  } else if (strcasecmp(optarg, "user") == 0) {
				  es |= NSUserDomainMask;
			  } else if (strcasecmp(optarg, "local") == 0) {
				  es |= NSLocalDomainMask;
			  } else if (strcasecmp(optarg, "network") == 0) {
				  es |= NSNetworkDomainMask;
			  } else if (strcasecmp(optarg, "system") == 0) {
				  es |= NSSystemDomainMask;
			  } else {
				badopts = true;
			  }
			  break;
		case '?':
		default:
			badopts = true;
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (lus.session_type == NULL)
		es &= ~NSUserDomainMask;

	if (argc == 0 && es == 0)
		badopts = true;

	if (badopts) {
		fprintf(stderr, "usage: %s load [-wF] [-D <user|local|network|system|all>] paths...\n", getprogname());
		return 1;
	}

	/* I wish I didn't need to do three passes, but I need to load mDNSResponder and use it too.
	 * And loading legacy mach init jobs is extra fun.
	 *
	 * In later versions of launchd, I hope to load everything in the first pass,
	 * then do the Bonjour magic on the jobs that need it, and reload them, but for now,
	 * I haven't thought through the various complexities of reloading jobs, and therefore
	 * launchd doesn't have reload support right now.
	 */

	lus.pass0 = launch_data_alloc(LAUNCH_DATA_ARRAY);
	lus.pass1 = launch_data_alloc(LAUNCH_DATA_ARRAY);
	lus.pass2 = launch_data_alloc(LAUNCH_DATA_ARRAY);

	es = NSStartSearchPathEnumeration(NSLibraryDirectory, es);

	while ((es = NSGetNextSearchPathEnumeration(es, nspath))) {
		glob_t g;

		if (lus.session_type) {
			strcat(nspath, "/LaunchAgents");
		} else {
			strcat(nspath, "/LaunchDaemons");
		}

		if (glob(nspath, GLOB_TILDE|GLOB_NOSORT, NULL, &g) == 0) {
			for (i = 0; i < g.gl_pathc; i++) {
				readpath(g.gl_pathv[i], &lus);
			}
			globfree(&g);
		}
	}

	for (i = 0; i < (size_t)argc; i++)
		readpath(argv[i], &lus);

	if (launch_data_array_get_count(lus.pass0) == 0 &&
			launch_data_array_get_count(lus.pass1) == 0 &&
			launch_data_array_get_count(lus.pass2) == 0) {
		fprintf(stderr, "nothing found to %s\n", lus.load ? "load" : "unload");
		launch_data_free(lus.pass0);
		launch_data_free(lus.pass1);
		launch_data_free(lus.pass2);
		return 1;
	}
	
	if (lus.load) {
		distill_jobs(lus.pass1);
		submit_mach_jobs(lus.pass0);
		submit_job_pass(lus.pass1);
		let_go_of_mach_jobs(lus.pass0);
		distill_jobs(lus.pass2);
		submit_job_pass(lus.pass2);
	} else {
		for (i = 0; i < launch_data_array_get_count(lus.pass1); i++)
			unloadjob(launch_data_array_get_index(lus.pass1, i));
		for (i = 0; i < launch_data_array_get_count(lus.pass2); i++)
			unloadjob(launch_data_array_get_index(lus.pass2, i));
	}

	return 0;
}

void
submit_mach_jobs(launch_data_t jobs)
{
	size_t i, c;

	c = launch_data_array_get_count(jobs);

	for (i = 0; i < c; i++) {
		launch_data_t tmp, oai = launch_data_array_get_index(jobs, i);
		const char *sn = NULL, *cmd = NULL;
		bool d = true;
		mach_port_t msr, msv;
		kern_return_t kr;
		uid_t u = getuid();

		if ((tmp = launch_data_dict_lookup(oai, MACHINIT_JOBKEY_ONDEMAND)))
			d = launch_data_get_bool(tmp);
		if ((tmp = launch_data_dict_lookup(oai, MACHINIT_JOBKEY_SERVICENAME)))
			sn = launch_data_get_string(tmp);
		if ((tmp = launch_data_dict_lookup(oai, MACHINIT_JOBKEY_COMMAND)))
			cmd = launch_data_get_string(tmp);

		if ((kr = bootstrap_create_server(bootstrap_port, (char *)cmd, u, d, &msr)) != KERN_SUCCESS) {
			fprintf(stderr, "%s: bootstrap_create_server(): %d\n", getprogname(), kr);
			continue;
		}
		if ((kr = bootstrap_create_service(msr, (char*)sn, &msv)) != KERN_SUCCESS) {
			fprintf(stderr, "%s: bootstrap_create_service(): %d\n", getprogname(), kr);
			mach_port_destroy(mach_task_self(), msr);
			continue;
		}
		launch_data_dict_insert(oai, launch_data_new_machport(msr), MACHINIT_JOBKEY_SERVERPORT);
		launch_data_dict_insert(oai, launch_data_new_machport(msv), MACHINIT_JOBKEY_SERVICEPORT);
	}
}

void
let_go_of_mach_jobs(launch_data_t jobs)
{
	size_t i, c = launch_data_array_get_count(jobs);

	for (i = 0; i < c; i++) {
		launch_data_t tmp, oai = launch_data_array_get_index(jobs, i);
		if ((tmp = launch_data_dict_lookup(oai, MACHINIT_JOBKEY_SERVICEPORT))) {
			mach_port_destroy(mach_task_self(), launch_data_get_machport(tmp));
		} else {
			fprintf(stderr, "%s: ack! missing service port!\n", getprogname());
		}
		if ((tmp = launch_data_dict_lookup(oai, MACHINIT_JOBKEY_SERVERPORT))) {
			mach_port_destroy(mach_task_self(), launch_data_get_machport(tmp));
		} else {
			fprintf(stderr, "%s: ack! missing server port!\n", getprogname());
		}
	}
}

void
submit_job_pass(launch_data_t jobs)
{
	launch_data_t msg, resp;
	size_t i;
	int e;

	if (launch_data_array_get_count(jobs) == 0)
		return;

	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);

	launch_data_dict_insert(msg, jobs, LAUNCH_KEY_SUBMITJOB);

	resp = launch_msg(msg);

	if (resp) {
		switch (launch_data_get_type(resp)) {
		case LAUNCH_DATA_ERRNO:
			if ((e = launch_data_get_errno(resp)))
				fprintf(stderr, "%s\n", strerror(e));
			break;
		case LAUNCH_DATA_ARRAY:
			for (i = 0; i < launch_data_array_get_count(jobs); i++) {
				launch_data_t obatind = launch_data_array_get_index(resp, i);
				launch_data_t jatind = launch_data_array_get_index(jobs, i);
				const char *lab4job = launch_data_get_string(launch_data_dict_lookup(jatind, LAUNCH_JOBKEY_LABEL));
				if (LAUNCH_DATA_ERRNO == launch_data_get_type(obatind)) {
					e = launch_data_get_errno(obatind);
					switch (e) {
					case EEXIST:
						fprintf(stderr, "%s: %s\n", lab4job, "Already loaded");
						break;
					case ESRCH:
						fprintf(stderr, "%s: %s\n", lab4job, "Not loaded");
						break;
					default:
						fprintf(stderr, "%s: %s\n", lab4job, strerror(e));
					case 0:
						break;
					}
				}
			}
			break;
		default:
			fprintf(stderr, "unknown respose from launchd!\n");
			break;
		}
		launch_data_free(resp);
	} else {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
	}

	launch_data_free(msg);
}

int
start_stop_remove_cmd(int argc, char *const argv[])
{
	launch_data_t resp, msg;
	const char *lmsgcmd = LAUNCH_KEY_STOPJOB;
	int e, r = 0;

	if (0 == strcmp(argv[0], "start"))
		lmsgcmd = LAUNCH_KEY_STARTJOB;

	if (0 == strcmp(argv[0], "remove"))
		lmsgcmd = LAUNCH_KEY_REMOVEJOB;

	if (argc != 2) {
		fprintf(stderr, "usage: %s %s <job label>\n", getprogname(), argv[0]);
		return 1;
	}

	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	launch_data_dict_insert(msg, launch_data_new_string(argv[1]), lmsgcmd);

	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_ERRNO) {
		if ((e = launch_data_get_errno(resp))) {
			fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], strerror(e));
			r = 1;
		}
	} else {
		fprintf(stderr, "%s %s returned unknown response\n", getprogname(), argv[0]);
		r = 1;
	}

	launch_data_free(resp);
	return r;
}

void
print_jobs(launch_data_t j)
{
	static size_t depth = 0;
	launch_data_t lo = launch_data_dict_lookup(j, LAUNCH_JOBKEY_LABEL);
	launch_data_t pido = launch_data_dict_lookup(j, LAUNCH_JOBKEY_PID);
	launch_data_t stato = launch_data_dict_lookup(j, LAUNCH_JOBKEY_LASTEXITSTATUS);
	launch_data_t sjobs = launch_data_dict_lookup(j, LAUNCH_JOBKEY_SUBJOBS);
	const char *label = launch_data_get_string(lo);
	size_t i, c;

	if (pido) {
		fprintf(stdout, "%lld\t-\t", launch_data_get_integer(pido));
	} else if (stato) {
		int wstatus = (int)launch_data_get_integer(stato);
		if (WIFEXITED(wstatus)) {
			fprintf(stdout, "-\t%d\t", WEXITSTATUS(wstatus));
		} else if (WIFSIGNALED(wstatus)) {
			fprintf(stdout, "-\t-%d\t", WTERMSIG(wstatus));
		} else {
			fprintf(stdout, "-\t???\t");
		}
	} else {
		fprintf(stdout, "-\t-\t");
	}
	for (i = 0; i < depth; i++)
		fprintf(stdout, "\t");

	fprintf(stdout, "%s\n", label);

	if (sjobs) {
		launch_data_t oai;

		c = launch_data_array_get_count(sjobs);

		depth++;
		for (i = 0; i < c; i++) {
			oai = launch_data_array_get_index(sjobs, i);
			print_jobs(oai);
		}
		depth--;
	}
}

void
print_obj(launch_data_t obj, const char *key, void *context __attribute__((unused)))
{
	static size_t indent = 0;
	size_t i, c;

	for (i = 0; i < indent; i++)
		fprintf(stdout, "\t");

	if (key)
		fprintf(stdout, "\"%s\" = ", key);

	switch (launch_data_get_type(obj)) {
	case LAUNCH_DATA_STRING:
		fprintf(stdout, "\"%s\";\n", launch_data_get_string(obj));
		break;
	case LAUNCH_DATA_INTEGER:
		fprintf(stdout, "%lld;\n", launch_data_get_integer(obj));
		break;
	case LAUNCH_DATA_REAL:
		fprintf(stdout, "%f;\n", launch_data_get_real(obj));
		break;
	case LAUNCH_DATA_BOOL:
		fprintf(stdout, "%s;\n", launch_data_get_bool(obj) ? "true" : "false");
		break;
	case LAUNCH_DATA_ARRAY:
		c = launch_data_array_get_count(obj);
		fprintf(stdout, "(\n");
		indent++;
		for (i = 0; i < c; i++)
			print_obj(launch_data_array_get_index(obj, i), NULL, NULL);
		indent--;
		for (i = 0; i < indent; i++)
			fprintf(stdout, "\t");
		fprintf(stdout, ");\n");
		break;
	case LAUNCH_DATA_DICTIONARY:
		fprintf(stdout, "{\n");
		indent++;
		launch_data_dict_iterate(obj, print_obj, NULL);
		indent--;
		for (i = 0; i < indent; i++)
			fprintf(stdout, "\t");
		fprintf(stdout, "};\n");
		break;
	case LAUNCH_DATA_FD:
		fprintf(stdout, "file-descriptor-object;\n");
		break;
	case LAUNCH_DATA_MACHPORT:
		fprintf(stdout, "mach-port-object;\n");
		break;
	default:
		fprintf(stdout, "???;\n");
		break;
	}
}

int
list_cmd(int argc, char *const argv[])
{
	launch_data_t resp, msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	int r = 0;

	if (argc > 2) {
		fprintf(stderr, "usage: %s list [label]\n", getprogname());
		return 1;
	} else if (argc == 2) {
		launch_data_dict_insert(msg, launch_data_new_string(argv[1]), LAUNCH_KEY_GETJOB);
	} else {
		launch_data_dict_insert(msg, launch_data_new_string(""), LAUNCH_KEY_GETJOB);
	}

	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_DICTIONARY) {
		if (argc == 1) {
			fprintf(stdout, "PID\tStatus\tLabel\n");
			print_jobs(resp);
		} else {
			print_obj(resp, NULL, NULL);
		}
	} else {
		fprintf(stderr, "%s %s returned unknown response\n", getprogname(), argv[0]);
		r = 1;
	}

	launch_data_free(resp);

	return r;
}

int
stdio_cmd(int argc, char *const argv[])
{
	launch_data_t resp, msg, tmp;
	int e, fd = -1, r = 0;

	if (argc != 2) {
		fprintf(stderr, "usage: %s %s <path>\n", getprogname(), argv[0]);
		return 1;
	}

	fd = open(argv[1], O_CREAT|O_APPEND|O_WRONLY|O_NOCTTY, DEFFILEMODE);

	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);

	if (fd == -1) {
		tmp = launch_data_new_string(argv[1]);
	} else {
		tmp = launch_data_new_fd(fd);
	}

	if (!strcmp(argv[0], "stdout")) {
		launch_data_dict_insert(msg, tmp, LAUNCH_KEY_SETSTDOUT);
	} else {
		launch_data_dict_insert(msg, tmp, LAUNCH_KEY_SETSTDERR);
	}

	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_ERRNO) {
		if ((e = launch_data_get_errno(resp))) {
			fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], strerror(e));
			r = 1;
		}
	} else {
		fprintf(stderr, "%s %s returned unknown response\n", getprogname(), argv[0]);
		r = 1;
	}

	if (fd != -1)
		close(fd);

	launch_data_free(resp);

	return r;
}

int
fyi_cmd(int argc, char *const argv[])
{
	launch_data_t resp, msg;
	const char *lmsgk = LAUNCH_KEY_RELOADTTYS;
	int e, r = 0;

	if (argc != 1) {
		fprintf(stderr, "usage: %s %s\n", getprogname(), argv[0]);
		return 1;
	}

	if (!strcmp(argv[0], "shutdown")) {
		lmsgk = LAUNCH_KEY_SHUTDOWN;
	} else if (!strcmp(argv[0], "singleuser")) {
		lmsgk = LAUNCH_KEY_SINGLEUSER;
	}

	msg = launch_data_new_string(lmsgk);
	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_ERRNO) {
		if ((e = launch_data_get_errno(resp))) {
			fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], strerror(e));
			r = 1;
		}
	} else {
		fprintf(stderr, "%s %s returned unknown response\n", getprogname(), argv[0]);
		r = 1;
	}

	launch_data_free(resp);

	return r;
}

int
logupdate_cmd(int argc, char *const argv[])
{
	launch_data_t resp, msg;
	int e, i, j, r = 0, m = 0;
	bool badargs = false, maskmode = false, onlymode = false, levelmode = false;
	const char *whichcmd = LAUNCH_KEY_SETLOGMASK;
	static const struct {
		const char *name;
		int level;
	} logtbl[] = {
		{ "debug",	LOG_DEBUG },
		{ "info",	LOG_INFO },
		{ "notice",	LOG_NOTICE },
		{ "warning",	LOG_WARNING },
		{ "error",	LOG_ERR },
		{ "critical",	LOG_CRIT },
		{ "alert",	LOG_ALERT },
		{ "emergency",	LOG_EMERG },
	};
	int logtblsz = sizeof logtbl / sizeof logtbl[0];

	if (argc >= 2) {
		if (!strcmp(argv[1], "mask"))
			maskmode = true;
		else if (!strcmp(argv[1], "only"))
			onlymode = true;
		else if (!strcmp(argv[1], "level"))
			levelmode = true;
		else
			badargs = true;
	}

	if (maskmode)
		m = LOG_UPTO(LOG_DEBUG);

	if (argc > 2 && (maskmode || onlymode)) {
		for (i = 2; i < argc; i++) {
			for (j = 0; j < logtblsz; j++) {
				if (!strcmp(argv[i], logtbl[j].name)) {
					if (maskmode)
						m &= ~(LOG_MASK(logtbl[j].level));
					else
						m |= LOG_MASK(logtbl[j].level);
					break;
				}
			}
			if (j == logtblsz) {
				badargs = true;
				break;
			}
		}
	} else if (argc > 2 && levelmode) {
		for (j = 0; j < logtblsz; j++) {
			if (!strcmp(argv[2], logtbl[j].name)) {
				m = LOG_UPTO(logtbl[j].level);
				break;
			}
		}
		if (j == logtblsz)
			badargs = true;
	} else if (argc == 1) {
		whichcmd = LAUNCH_KEY_GETLOGMASK;
	} else {
		badargs = true;
	}

	if (badargs) {
		fprintf(stderr, "usage: %s [[mask loglevels...] | [only loglevels...] [level loglevel]]\n", getprogname());
		return 1;
	}

	if (whichcmd == LAUNCH_KEY_SETLOGMASK) {
		msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
		launch_data_dict_insert(msg, launch_data_new_integer(m), whichcmd);
	} else {
		msg = launch_data_new_string(whichcmd);
	}

	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_ERRNO) {
		if ((e = launch_data_get_errno(resp))) {
			fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], strerror(e));
			r = 1;
		}
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_INTEGER) {
		if (whichcmd == LAUNCH_KEY_GETLOGMASK) {
			m = launch_data_get_integer(resp);
			for (j = 0; j < logtblsz; j++) {
				if (m & LOG_MASK(logtbl[j].level))
					fprintf(stdout, "%s ", logtbl[j].name);
			}
			fprintf(stdout, "\n");
		}
	} else {
		fprintf(stderr, "%s %s returned unknown response\n", getprogname(), argv[0]);
		r = 1;
	}

	launch_data_free(resp);

	return r;
}

static const struct {
	const char *name;
	int lim;
} limlookup[] = {
	{ "cpu",	RLIMIT_CPU },
	{ "filesize",	RLIMIT_FSIZE },
	{ "data",	RLIMIT_DATA },
	{ "stack",	RLIMIT_STACK },
	{ "core",	RLIMIT_CORE },
	{ "rss", 	RLIMIT_RSS },
	{ "memlock",	RLIMIT_MEMLOCK },
	{ "maxproc",	RLIMIT_NPROC },
	{ "maxfiles",	RLIMIT_NOFILE }
};

static const size_t limlookupcnt = sizeof limlookup / sizeof limlookup[0];

ssize_t
name2num(const char *n)
{
	size_t i;

	for (i = 0; i < limlookupcnt; i++) {
		if (!strcmp(limlookup[i].name, n)) {
			return limlookup[i].lim;
		}
	}
	return -1;
}

const char *
num2name(int n)
{
	size_t i;

	for (i = 0; i < limlookupcnt; i++) {
		if (limlookup[i].lim == n)
			return limlookup[i].name;
	}
	return NULL;
}

const char *
lim2str(rlim_t val, char *buf)
{
	if (val == RLIM_INFINITY)
		strcpy(buf, "unlimited");
	else
		sprintf(buf, "%lld", val);
	return buf;
}

bool
str2lim(const char *buf, rlim_t *res)
{
	char *endptr;
	*res = strtoll(buf, &endptr, 10);
	if (!strcmp(buf, "unlimited")) {
		*res = RLIM_INFINITY;
		return false;
	} else if (*endptr == '\0') {
		 return false;
	}
	return true;
}

int
limit_cmd(int argc __attribute__((unused)), char *const argv[])
{
	char slimstr[100];
	char hlimstr[100];
	struct rlimit *lmts = NULL;
	launch_data_t resp, resp1 = NULL, msg, tmp;
	int r = 0;
	size_t i, lsz = -1;
	ssize_t which = 0;
	rlim_t slim = -1, hlim = -1;
	bool badargs = false;

	if (argc > 4)
		badargs = true;

	if (argc >= 3 && str2lim(argv[2], &slim))
		badargs = true;
	else
		hlim = slim;

	if (argc == 4 && str2lim(argv[3], &hlim))
		badargs = true;

	if (argc >= 2 && -1 == (which = name2num(argv[1])))
		badargs = true;

	if (badargs) {
		fprintf(stderr, "usage: %s %s [", getprogname(), argv[0]);
		for (i = 0; i < limlookupcnt; i++)
			fprintf(stderr, "%s %s", limlookup[i].name, (i + 1) == limlookupcnt ? "" : "| ");
		fprintf(stderr, "[both | soft hard]]\n");
		return 1;
	}

	msg = launch_data_new_string(LAUNCH_KEY_GETRESOURCELIMITS);
	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_OPAQUE) {
		lmts = launch_data_get_opaque(resp);
		lsz = launch_data_get_opaque_size(resp);
		if (argc <= 2) {
			for (i = 0; i < (lsz / sizeof(struct rlimit)); i++) {
				if (argc == 2 && (size_t)which != i)
					continue;
				fprintf(stdout, "\t%-12s%-15s%-15s\n", num2name(i),
						lim2str(lmts[i].rlim_cur, slimstr),
						lim2str(lmts[i].rlim_max, hlimstr));
			}
		}
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_STRING) {
		fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], launch_data_get_string(resp));
		r = 1;
	} else {
		fprintf(stderr, "%s %s returned unknown response\n", getprogname(), argv[0]);
		r = 1;
	}

	if (argc <= 2 || r != 0) {
		launch_data_free(resp);
		return r;
	} else {
		resp1 = resp;
	}

	lmts[which].rlim_cur = slim;
	lmts[which].rlim_max = hlim;

	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	tmp = launch_data_new_opaque(lmts, lsz);
	launch_data_dict_insert(msg, tmp, LAUNCH_KEY_SETRESOURCELIMITS);
	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_STRING) {
		fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], launch_data_get_string(resp));
		r = 1;
	} else if (launch_data_get_type(resp) != LAUNCH_DATA_OPAQUE) {
		fprintf(stderr, "%s %s returned unknown response\n", getprogname(), argv[0]);
		r = 1;
	}

	launch_data_free(resp);
	launch_data_free(resp1);

	return r;
}

int
umask_cmd(int argc, char *const argv[])
{
	launch_data_t resp, msg;
	bool badargs = false;
	char *endptr;
	long m = 0;
	int r = 0;

	if (argc == 2) {
		m = strtol(argv[1], &endptr, 8);
		if (*endptr != '\0' || m > 0777)
			badargs = true;
	}

	if (argc > 2 || badargs) {
		fprintf(stderr, "usage: %s %s <mask>\n", getprogname(), argv[0]);
		return 1;
	}


	if (argc == 1) {
		msg = launch_data_new_string(LAUNCH_KEY_GETUMASK);
	} else {
		msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
		launch_data_dict_insert(msg, launch_data_new_integer(m), LAUNCH_KEY_SETUMASK);
	}
	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_STRING) {
		fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], launch_data_get_string(resp));
		r = 1;
	} else if (launch_data_get_type(resp) != LAUNCH_DATA_INTEGER) {
		fprintf(stderr, "%s %s returned unknown response\n", getprogname(), argv[0]);
		r = 1;
	} else if (argc == 1) {
		fprintf(stdout, "%o\n", (unsigned int)launch_data_get_integer(resp));
	}

	launch_data_free(resp);

	return r;
}

int
submit_cmd(int argc, char *const argv[])
{
	launch_data_t msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	launch_data_t job = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	launch_data_t resp, largv = launch_data_alloc(LAUNCH_DATA_ARRAY);
	int ch, i, r = 0;

	launch_data_dict_insert(job, launch_data_new_bool(false), LAUNCH_JOBKEY_ONDEMAND);

	while ((ch = getopt(argc, argv, "l:p:o:e:")) != -1) {
		switch (ch) {
		case 'l':
			launch_data_dict_insert(job, launch_data_new_string(optarg), LAUNCH_JOBKEY_LABEL);
			break;
		case 'p':
			launch_data_dict_insert(job, launch_data_new_string(optarg), LAUNCH_JOBKEY_PROGRAM);
			break;
		case 'o':
			launch_data_dict_insert(job, launch_data_new_string(optarg), LAUNCH_JOBKEY_STANDARDOUTPATH);
			break;
		case 'e':
			launch_data_dict_insert(job, launch_data_new_string(optarg), LAUNCH_JOBKEY_STANDARDERRORPATH);
			break;
		default:
			fprintf(stderr, "usage: %s submit ...\n", getprogname());
			return 1;
		}
	}
	argc -= optind;
	argv += optind;

	for (i = 0; argv[i]; i++) {
		launch_data_array_append(largv, launch_data_new_string(argv[i]));
	}

	launch_data_dict_insert(job, largv, LAUNCH_JOBKEY_PROGRAMARGUMENTS);

	launch_data_dict_insert(msg, job, LAUNCH_KEY_SUBMITJOB);

	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_ERRNO) {
		errno = launch_data_get_errno(resp);
		if (errno) {
			fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], strerror(errno));
			r = 1;
		}
	} else {
		fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], "unknown response");
	}

	launch_data_free(resp);

	return r;
}

int
getrusage_cmd(int argc, char *const argv[])
{
	launch_data_t resp, msg;
	bool badargs = false;
	int r = 0;

	if (argc != 2)
		badargs = true;
	else if (strcmp(argv[1], "self") && strcmp(argv[1], "children"))
		badargs = true;

	if (badargs) {
		fprintf(stderr, "usage: %s %s self | children\n", getprogname(), argv[0]);
		return 1;
	}

	if (!strcmp(argv[1], "self")) {
		msg = launch_data_new_string(LAUNCH_KEY_GETRUSAGESELF);
	} else {
		msg = launch_data_new_string(LAUNCH_KEY_GETRUSAGECHILDREN);
	}

	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_ERRNO) {
		fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], strerror(launch_data_get_errno(resp)));
		r = 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_OPAQUE) {
		struct rusage *rusage = launch_data_get_opaque(resp);
		fprintf(stdout, "\t%-10f\tuser time used\n",
				(double)rusage->ru_utime.tv_sec + (double)rusage->ru_utime.tv_usec / (double)1000000);
		fprintf(stdout, "\t%-10f\tsystem time used\n",
				(double)rusage->ru_stime.tv_sec + (double)rusage->ru_stime.tv_usec / (double)1000000);
		fprintf(stdout, "\t%-10ld\tmax resident set size\n", rusage->ru_maxrss);
		fprintf(stdout, "\t%-10ld\tshared text memory size\n", rusage->ru_ixrss);
		fprintf(stdout, "\t%-10ld\tunshared data size\n", rusage->ru_idrss);
		fprintf(stdout, "\t%-10ld\tunshared stack size\n", rusage->ru_isrss);
		fprintf(stdout, "\t%-10ld\tpage reclaims\n", rusage->ru_minflt);
		fprintf(stdout, "\t%-10ld\tpage faults\n", rusage->ru_majflt);
		fprintf(stdout, "\t%-10ld\tswaps\n", rusage->ru_nswap);
		fprintf(stdout, "\t%-10ld\tblock input operations\n", rusage->ru_inblock);
		fprintf(stdout, "\t%-10ld\tblock output operations\n", rusage->ru_oublock);
		fprintf(stdout, "\t%-10ld\tmessages sent\n", rusage->ru_msgsnd);
		fprintf(stdout, "\t%-10ld\tmessages received\n", rusage->ru_msgrcv);
		fprintf(stdout, "\t%-10ld\tsignals received\n", rusage->ru_nsignals);
		fprintf(stdout, "\t%-10ld\tvoluntary context switches\n", rusage->ru_nvcsw);
		fprintf(stdout, "\t%-10ld\tinvoluntary context switches\n", rusage->ru_nivcsw);
	} else {
		fprintf(stderr, "%s %s returned unknown response\n", getprogname(), argv[0]);
		r = 1;
	} 

	launch_data_free(resp);

	return r;
}

bool
launch_data_array_append(launch_data_t a, launch_data_t o)
{
	size_t offt = launch_data_array_get_count(a);

	return launch_data_array_set_index(a, o, offt);
}

mach_port_t
str2bsport(const char *s)
{
	bool getrootbs = strcmp(s, "/") == 0;
	mach_port_t last_bport, bport = bootstrap_port;
	task_t task = mach_task_self();
	kern_return_t result;

	if (strcmp(s, "..") == 0 || getrootbs) {
		do {
			last_bport = bport;
			result = bootstrap_parent(last_bport, &bport);

			if (result == BOOTSTRAP_NOT_PRIVILEGED) {
				fprintf(stderr, "Permission denied\n");
				return 1;
			} else if (result != BOOTSTRAP_SUCCESS) {
				fprintf(stderr, "bootstrap_parent() %d\n", result);
				return 1;
			}
		} while (getrootbs && last_bport != bport);
	} else {
		int pid = atoi(s);

		result = task_for_pid(mach_task_self(), pid, &task);

		if (result != KERN_SUCCESS) {
			fprintf(stderr, "task_for_pid() %s\n", mach_error_string(result));
			return 1;
		}

		result = task_get_bootstrap_port(task, &bport);

		if (result != KERN_SUCCESS) {
			fprintf(stderr, "Couldn't get bootstrap port: %s\n", mach_error_string(result));
			return 1;
		}
	}

	return bport;
}

int
bsexec_cmd(int argc, char *const argv[])
{
	kern_return_t result;
	mach_port_t bport;

	if (argc < 3) {
		fprintf(stderr, "usage: %s bsexec <PID> prog...\n", getprogname());
		return 1;
	}

	bport = str2bsport(argv[1]);

	result = task_set_bootstrap_port(mach_task_self(), bport);

	if (result != KERN_SUCCESS) {
		fprintf(stderr, "Couldn't switch to new bootstrap port: %s\n", mach_error_string(result));
		return 1;
	}

	setgid(getgid());
	setuid(getuid());

	execvp(argv[2], argv + 2);
	fprintf(stderr, "execvp(): %s\n", strerror(errno));
	return 1;
}

int
bslist_cmd(int argc, char *const argv[])
{
	kern_return_t result;
	mach_port_t bport = bootstrap_port;
	name_array_t service_names;
	mach_msg_type_number_t service_cnt, service_active_cnt;
	bootstrap_status_array_t service_actives;
	unsigned int i;

	if (argc == 2)
		bport = str2bsport(argv[1]);

	if (bport == MACH_PORT_NULL) {
		fprintf(stderr, "Invalid bootstrap port\n");
		return 1;
	}

	result = bootstrap_info(bport, &service_names, &service_cnt, &service_actives, &service_active_cnt);
	if (result != BOOTSTRAP_SUCCESS) {
		fprintf(stderr, "bootstrap_info(): %d\n", result);
		return 1;
	}

#define bport_state(x)	(((x) == BOOTSTRAP_STATUS_ACTIVE) ? "A" : ((x) == BOOTSTRAP_STATUS_ON_DEMAND) ? "D" : "I")

	for (i = 0; i < service_cnt ; i++)
		fprintf(stdout, "%-3s%s\n", bport_state((service_actives[i])), service_names[i]);

	return 0;
}

bool
is_legacy_mach_job(launch_data_t obj)
{
	bool has_servicename = launch_data_dict_lookup(obj, MACHINIT_JOBKEY_SERVICENAME);
	bool has_command  = launch_data_dict_lookup(obj, MACHINIT_JOBKEY_COMMAND);
	bool has_label = launch_data_dict_lookup(obj, LAUNCH_JOBKEY_LABEL);

	return has_command && has_servicename && !has_label;
}

void
_log_launchctl_bug(const char *rcs_rev, const char *path, unsigned int line, const char *test)
{
	int saved_errno = errno;
	char buf[100];
	const char *file = strrchr(path, '/');
	char *rcs_rev_tmp = strchr(rcs_rev, ' ');

	if (!file) {
		file = path;
	} else {
		file += 1;
	}

	if (!rcs_rev_tmp) {
		strlcpy(buf, rcs_rev, sizeof(buf));
	} else {
		strlcpy(buf, rcs_rev_tmp + 1, sizeof(buf));
		rcs_rev_tmp = strchr(buf, ' ');
		if (rcs_rev_tmp)
			*rcs_rev_tmp = '\0';
	}

	fprintf(stderr, "Bug: %s:%u (%s):%u: %s\n", file, line, buf, saved_errno, test);
}

void
loopback_setup(void)
{
	struct ifaliasreq ifra;
	struct in6_aliasreq ifra6;
	struct ifreq ifr;
	int s, s6;

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, "lo0");

	assumes((s = socket(AF_INET, SOCK_DGRAM, 0)) != -1);
	assumes((s6 = socket(AF_INET6, SOCK_DGRAM, 0)) != -1);

	if (assumes(ioctl(s, SIOCGIFFLAGS, &ifr) != -1)) {
		ifr.ifr_flags |= IFF_UP;
		assumes(ioctl(s, SIOCSIFFLAGS, &ifr) != -1);
	}

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, "lo0");

	if (assumes(ioctl(s6, SIOCGIFFLAGS, &ifr) != -1)) {
		ifr.ifr_flags |= IFF_UP;
		assumes(ioctl(s6, SIOCSIFFLAGS, &ifr) != -1);
	}

	memset(&ifra, 0, sizeof(ifra));
	strcpy(ifra.ifra_name, "lo0");
	((struct sockaddr_in *)&ifra.ifra_addr)->sin_family = AF_INET;
	((struct sockaddr_in *)&ifra.ifra_addr)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	((struct sockaddr_in *)&ifra.ifra_addr)->sin_len = sizeof(struct sockaddr_in);
	((struct sockaddr_in *)&ifra.ifra_mask)->sin_family = AF_INET;
	((struct sockaddr_in *)&ifra.ifra_mask)->sin_addr.s_addr = htonl(IN_CLASSA_NET);
	((struct sockaddr_in *)&ifra.ifra_mask)->sin_len = sizeof(struct sockaddr_in);

	assumes(ioctl(s, SIOCAIFADDR, &ifra) != -1);

	memset(&ifra6, 0, sizeof(ifra6));
	strcpy(ifra6.ifra_name, "lo0");

	ifra6.ifra_addr.sin6_family = AF_INET6;
	ifra6.ifra_addr.sin6_addr = in6addr_loopback;
	ifra6.ifra_addr.sin6_len = sizeof(struct sockaddr_in6);
	ifra6.ifra_prefixmask.sin6_family = AF_INET6;
	memset(&ifra6.ifra_prefixmask.sin6_addr, 0xff, sizeof(struct in6_addr));
	ifra6.ifra_prefixmask.sin6_len = sizeof(struct sockaddr_in6);
	ifra6.ifra_lifetime.ia6t_vltime = ND6_INFINITE_LIFETIME;
	ifra6.ifra_lifetime.ia6t_pltime = ND6_INFINITE_LIFETIME;

	assumes(ioctl(s6, SIOCAIFADDR_IN6, &ifra6) != -1);

	assumes(close(s) == 0);
	assumes(close(s6) == 0);
}

pid_t
fwexec(const char *const *argv, bool _wait)
{
	int wstatus;
	pid_t p;

	switch ((p = fork())) {
	case -1:
		break;
	case 0:
		setsid();
		execvp(argv[0], (char *const *)argv);
		_exit(EXIT_FAILURE);
		break;
	default:
		if (!_wait)
			return p;
		if (p == waitpid(p, &wstatus, 0)) {
			if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == EXIT_SUCCESS)
				return p;
		}
		break;
	}

	return -1;
}

void
do_potential_fsck(void)
{
	const char *safe_fsck_tool[] = { "fsck", "-fy", NULL };
	const char *fsck_tool[] = { "fsck", "-p", NULL };
	const char *remount_tool[] = { "mount", "-uw", "/", NULL };
	struct statfs sfs;

	if (assumes(statfs("/", &sfs) != -1)) {
		if (!(sfs.f_flags & MNT_RDONLY)) {
			fprintf(stdout, "Root file system is read-write, skipping fsck.\n");
			return;
		}
	}

	if (!is_safeboot()) {
		if (fwexec(fsck_tool, true) != -1)
			goto out;
	}

	if (fwexec(safe_fsck_tool, true) != -1) {
		goto out;
	}

	fprintf(stderr, "fsck failed! Leaving the root file system read-only...\n");

	return;
out:
	assumes(fwexec(remount_tool, true) != -1);
}

bool
path_check(const char *path)
{
	struct stat sb;

	if (stat(path, &sb) == 0)
		return true;
	return false;
}

bool
is_safeboot(void)
{
	int sbmib[] = { CTL_KERN, KERN_SAFEBOOT };
	uint32_t sb = 0;
	size_t sbsz = sizeof(sb);

	if (!assumes(sysctl(sbmib, 2, &sb, &sbsz, NULL, 0) == 0))
		return false;

	return (bool)sb;
}

bool
is_netboot(void)
{
	int nbmib[] = { CTL_KERN, KERN_NETBOOT };
	uint32_t nb = 0;
	size_t nbsz = sizeof(nb);

	if (!assumes(sysctl(nbmib, 2, &nb, &nbsz, NULL, 0) == 0))
		return false;

	return (bool)nb;
}

void
apply_func_to_dir(const char *thedir, void (*thefunc)(const char *))
{
	struct dirent *de;
	DIR *od;
	int currend_dir_fd;

	if (!assumes((currend_dir_fd = open(".", 0)) != -1))
		return;

	if (!assumes(chdir(thedir) != -1))
		goto out;

	if (!assumes(od = opendir(".")))
		goto out;

	while ((de = readdir(od))) {
		struct stat sb;

		if (strcmp(de->d_name, ".") == 0)
			continue;
		if (strcmp(de->d_name, "..") == 0)
			continue;

		if (assumes(stat(de->d_name, &sb) != -1)) {
			if (S_ISDIR(sb.st_mode))
				apply_func_to_dir(de->d_name, thefunc);
			thefunc(de->d_name);
		}
	}

	assumes(closedir(od) != -1);

out:
	assumes(fchdir(currend_dir_fd) != -1);
	assumes(close(currend_dir_fd) != -1);
}

void
empty_dir(const char *path)
{
	assumes(chflags(path, 0) != -1);
	assumes(remove(path) != -1);
}

int
touch_file(const char *path, mode_t m)
{
	int fd = open(path, O_CREAT, m);

	if (fd == -1)
		return -1;

	return close(fd);
}

void
apply_sysctls_from_file(const char *thefile)
{
	const char *sysctl_tool[] = { "sysctl", "-w", NULL, NULL };
	size_t ln_len = 0;
	char *val, *tmpstr;
	FILE *sf;

	if (!(sf = fopen(thefile, "r")))
		return;

	while ((val = fgetln(sf, &ln_len))) {
		if (ln_len == 0)
			continue;
		if (!assumes((tmpstr = malloc(ln_len + 1)) != NULL))
			continue;
		memcpy(tmpstr, val, ln_len);
		tmpstr[ln_len] = 0;
		val = tmpstr;

		while (*val && isspace(*val))
			val++;
		if (*val == '\0' || *val == '#')
			goto skip_sysctl_tool;
		sysctl_tool[2] = val;
		assumes(fwexec(sysctl_tool, true) != -1);
skip_sysctl_tool:
		free(tmpstr);
	}

	assumes(fclose(sf) == 0);
}

void
do_sysversion_sysctl(void)
{
	int mib[] = { CTL_KERN, KERN_OSVERSION };
	CFDictionaryRef versdict;
	CFStringRef buildvers;
	char buf[1024];
	size_t bufsz = sizeof(buf);

	/* <rdar://problem/4477682> ER: launchd should set kern.osversion very early in boot */

	if (getuid() != 0)
		return;

	if (sysctl(mib, 2, buf, &bufsz, NULL, 0) == -1) {
		fprintf(stderr, "sysctl(): %s\n", strerror(errno));
		return;
	}

	if (buf[0] != '\0')
		return;

	versdict = _CFCopySystemVersionDictionary();
	buildvers = CFDictionaryGetValue(versdict, _kCFSystemVersionBuildVersionKey);
	CFStringGetCString(buildvers, buf, sizeof(buf), kCFStringEncodingUTF8);

	if (sysctl(mib, 2, NULL, 0, buf, strlen(buf) + 1) == -1) {
		fprintf(stderr, "sysctl(): %s\n", strerror(errno));
	}

	CFRelease(versdict);
}
