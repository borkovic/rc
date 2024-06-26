/* status.c: functions for printing fancy status messages in rc */

#include "rc.h"
#include "sigmsgs.h"
#include "statval.h"
#include "wait.h"

/* status == the wait() value of the last command in the pipeline, or the last command */

static void statprint(pid_t, int);

static int statuses[MAX_PIPELINE];
static int pipelength = 1;

/*
   Test to see if rc's status is true. According to td, status is true
   if and only if every pipe-member has an exit status of zero.
*/

extern int istrue() {
	int i;
	for (i = 0; i < pipelength; i++)
		if (statuses[i] != 0)
			return FALSE;
	return TRUE;
}

/*
   Return the status as an integer. A status which has low-bits set is
   a signal number, whereas a status with high bits set is a value set
   from exit(). The presence of a signal just sets status to 1. Also,
   a pipeline with nonzero exit statuses in it just sets status to 1.
*/

extern int getstatus() {
	int s;
	if (pipelength > 1)
		return !istrue();
	s = statuses[0];
	if (WIFSIGNALED(s))
		return 1;
	return WEXITSTATUS(s);
}

extern void setN(int code) {
	int scode = 0;
	switch (code) {
	case 0:
		scode = STATUS0;
		break;
	case 1:
		scode = STATUS1;
		break;
	case 2:
		scode = STATUS2;
		break;
	default:
		scode = STATUS1;
		break;
	}
	setstatus(-1, scode);
}

extern void set(bool code) {
	setstatus(-1, code ? STATUS0 : STATUS1);
}

/* set a simple status, as opposed to a pipeline */

extern void setstatus(pid_t pid, int stat) {
	pipelength = 1;
	statuses[0] = stat;
	statprint(pid, stat);
}

/* set number of statuses for pipeline */

extern void setpipestatuslength(int n) {
	pipelength = n;
}

/* set a status of a pipeline */

extern void setpipestatus(int i, pid_t pid, int stat) {
	statuses[i] = stat;
	statprint(pid, stat);
}

/*
   Print a message if called from the wait builitin in an interactive
   shell or termination was with a signal and it is not sigint and
   sigpipe or the child dumped core. Exit on error if -e is set.
*/

static void statprint(pid_t pid, int i) {
	int t = WIFSIGNALED(i) ? WTERMSIG(i) : 0;
	const char *core = ((t > 0) && myWIFDUMPED(i) ? "--core dumped" : "");
	if ((interactive && pid != -1) || (t > 0 && (*core || (t != SIGINT && t != SIGPIPE)))) {
		if (pid != -1)
			fprint(2, "%ld: ", (long)pid);
		if (t == 0)
			fprint(2, "done (%d)\n", WEXITSTATUS(i));
		else if ((t > 0) && (t < NUMOFSIGNALS) && *signals[t].msg != '\0')
			fprint(2, "%s%s\n", signals[t].msg, core);
		else
			fprint(2, "unknown signal %d%s\n", t, core);
	}
	if (i != 0 && dashee && !cond)
		rc_exit(getstatus());
}

/* prepare a list to be passed back. Used whenever $status is dereferenced */

extern List *sgetstatus() {
	List *r = NULL;
	int i;

	for (i = 0; i < pipelength; i++) {
		List *q = nnew(List);
		q->w = strstatus(statuses[i]);
		q->m = NULL;
		q->n = r;
		r = q;
	}

	return r;
}

/* return status as a string (used above and for bqstatus) */

extern char *strstatus(int s) {
	if (WIFSIGNALED(s)) {
		int t = WTERMSIG(s);
		const char *core = myWIFDUMPED(s) ? "+core" : "";
		if ((t > 0) && (t < NUMOFSIGNALS) && *signals[t].name != '\0')
			return nprint("%s%s", signals[t].name, core);
		else
			return nprint("-%d%s", t, core); /* unknown signals are negated */
	} else
		return nprint("%d", WEXITSTATUS(s));
}

extern void ssetstatus(char **av) {
	int i, j, k, l;
	bool found;
	bool have_err = FALSE;
	for (l = 0; av[l] != NULL; l++)
		; /* count up array length */
	--l;
	for (i = 0; av[i] != NULL; i++) {
        j = a2u(av[i]);
		if (j >= 0) {
			statuses[l - i] = j << 8;
			if (j > 0) {
				have_err = TRUE;
			}
			continue;
		}
		found = FALSE;
		for (k = 0; k < NUMOFSIGNALS; k++) {
			if (streq(signals[k].name, av[i])) {
				statuses[l - i] = k;
				found = TRUE;
				break;
			}
			else {
				size_t len = strlen(signals[k].name);
				if (strncmp_fast(signals[k].name, av[i], len) == 0 && streq(av[i] + len, "+core")) {
					statuses[l - i] = k + 0x80;
					found = TRUE;
					break;
				}
			}
		}
		if (!found) {
			fprint(2, "bad status\n");
			set(FALSE);
			return;
		}
		have_err = TRUE;
	}
	pipelength = i;
	if (have_err && dashee && !cond) {
		rc_exit(getstatus());
	}
}
