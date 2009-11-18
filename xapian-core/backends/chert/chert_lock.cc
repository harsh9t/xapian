/* chert_lock.cc: database locking for chert backend.
 *
 * Copyright (C) 2005,2006,2007,2008,2009 Olly Betts
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301
 * USA
 */

#include <config.h>

#include "chert_lock.h"

#ifndef __WIN32__
#include "safeerrno.h"

#include "safefcntl.h"
#include <unistd.h>
#include <cstdlib>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstring>
#endif

#include "omassert.h"

#ifdef __CYGWIN__
#include <sys/cygwin.h>
#endif

using namespace std;

ChertLock::reason
ChertLock::lock(bool exclusive, string & explanation) {
    // Currently we only support exclusive locks.
    (void)exclusive;
    Assert(exclusive);
#if defined __CYGWIN__ || defined __WIN32__
    Assert(hFile == INVALID_HANDLE_VALUE);
#ifdef __CYGWIN__
    char fnm[MAX_PATH];
    cygwin_conv_to_win32_path(filename.c_str(), fnm);
#else
    const char *fnm = filename.c_str();
#endif
    hFile = CreateFile(fnm, GENERIC_WRITE, FILE_SHARE_READ,
		       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) return SUCCESS;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return INUSE;
    explanation = "";
    return UNKNOWN;
#elif defined __EMX__
    APIRET rc;
    ULONG ulAction;
    rc = DosOpen((PCSZ)filename.c_str(), &hFile, &ulAction, 0, FILE_NORMAL,
		 OPEN_ACTION_OPEN_IF_EXISTS | OPEN_ACTION_CREATE_IF_NEW,
		 OPEN_SHARE_DENYWRITE | OPEN_ACCESS_WRITEONLY,
		 NULL);
    if (rc == NO_ERROR) return SUCCESS;
    if (rc == ERROR_ACCESS_DENIED) return INUSE;
    explanation = "";
    return UNKNOWN;
#else
    Assert(fd == -1);
    int lockfd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (lockfd < 0) {
	// Couldn't open lockfile.
	explanation = string("Couldn't open lockfile: ") + strerror(errno);
	return UNKNOWN;
    }

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, fds) < 0) {
	// Couldn't create socketpair.
	explanation = string("Couldn't create socketpair: ") + strerror(errno);
	close(lockfd);
	return UNKNOWN;
    }

    pid_t child = fork();

    if (child == 0) {
	// Child process.
	close(fds[0]);

	reason why = SUCCESS;
	{
	    struct flock fl;
	    fl.l_type = F_WRLCK;
	    fl.l_whence = SEEK_SET;
	    fl.l_start = 0;
	    fl.l_len = 1;
	    while (fcntl(lockfd, F_SETLK, &fl) == -1) {
		if (errno != EINTR) {
		    // Lock failed - translate known errno values into a reason
		    // code.
		    if (errno == EACCES || errno == EAGAIN) {
			why = INUSE;
		    } else if (errno == ENOLCK) {
			why = UNSUPPORTED;
		    } else {
			_exit(0);
		    }
		    break;
		}
	    }
	}

	{
	    // Tell the parent if we got the lock, and if not, why not.
	    char ch = static_cast<char>(why);
	    while (write(fds[1], &ch, 1) < 0) {
		// EINTR means a signal interrupted us, so retry.
		// Otherwise we're DOOMED!  The best we can do is just exit
		// and the parent process should get EOF and know the lock
		// failed.
		if (errno != EINTR) _exit(1);
	    }
	    if (why != SUCCESS) _exit(0);
	}

	// Connect pipe to stdin and stdout.
	dup2(fds[1], 0);
	dup2(fds[1], 1);

	// Make sure we don't block unmount() of partition holding the current
	// directory.
	if (chdir("/") < 0) {
	    // We can't usefully do anything in response to an error, so just
	    // ignore it - the worst harm it can do is make it impossible to
	    // unmount a partition.
	    //
	    // We need the if statement because glibc's _FORTIFY_SOURCE mode
	    // gives a warning even if we cast the result to void.
	}

	// Make sure we don't hang on to open files which may get deleted but
	// not have their disk space released until we exit.
	int maxfd = static_cast<int>(sysconf(_SC_OPEN_MAX));
	for (int i = 2; i < maxfd; ++i) {
	    if (i != lockfd) {
		// Retry on EINTR; just ignore other errors (we'll get
		// EBADF if the fd isn't open so that's OK).
		while (close(i) < 0 && errno == EINTR) { }
	    }
	}

	// FIXME: use special statically linked helper instead of cat.
	execl("/bin/cat", "/bin/cat", static_cast<void*>(NULL));
	// Emulate cat ourselves (we try to avoid this to reduce VM overhead).
	char ch;
	while (read(0, &ch, 1) != 0) { /* Do nothing */ }
	_exit(0);
    }

    close(lockfd);
    close(fds[1]);

    if (child == -1) {
	// Couldn't fork.
	explanation = string("Couldn't fork: ") + strerror(errno);
	close(fds[0]);
	return UNKNOWN;
    }

    reason why = UNKNOWN;

    // Parent process.
    while (true) {
	char ch;
	ssize_t n = read(fds[0], &ch, 1);
	if (n == 1) {
	    why = static_cast<reason>(ch);
	    if (why != SUCCESS) break;
	    // Got the lock.
	    fd = fds[0];
	    pid = child;
	    return SUCCESS;
	}
	if (n == 0) {
	    // EOF means the lock failed.
	    explanation.assign("Got EOF reading from child process");
	    break;
	}
	if (errno != EINTR) {
	    // Treat unexpected errors from read() as failure to get the lock.
	    explanation = string("Error reading from child process: ") + strerror(errno);
	    break;
	}
    }

    close(fds[0]);

    int status;
    while (waitpid(child, &status, 0) < 0) {
	if (errno != EINTR) break;
    }

    return why;
#endif
}

void
ChertLock::release() {
#if defined __CYGWIN__ || defined __WIN32__
    if (hFile == INVALID_HANDLE_VALUE) return;
    CloseHandle(hFile);
    hFile = INVALID_HANDLE_VALUE;
#elif defined __EMX__
    if (hFile == NULLHANDLE) return;
    DosClose(hFile);
    hFile = NULLHANDLE;
#else
    if (fd < 0) return;
    close(fd);
    fd = -1;
    // The only likely error from kill is ESRCH.  The other possibilities
    // (according to the Linux man page) are EINVAL (invalid signal) and EPERM
    // (don't have permission to SIGHUP the process) but in none of the cases
    // does calling waitpid do us any good!
    if (kill(pid, SIGHUP) == 0) {
	int status;
	while (waitpid(pid, &status, 0) < 0) {
	    if (errno != EINTR) break;
	}
    }
#endif
}