# Root's login shell.
#
# Ordinarily this does nothing. Its one job is the KDE session: kwin's DRM
# backend takes its devices from logind, logind only hands them to a registered
# session, and a session exists only if something logged in through PAM. So
# /etc/kde.sh re-executes itself through /sbin/login-pam, and this is where it
# lands -- inside the session pam_elogind.so has just created.
#
# The marker file is what distinguishes that from an ordinary interactive
# login: login sanitises the environment, so a variable exported before it does
# not survive. It is removed immediately, so a real login after this one gets a
# normal shell rather than another compositor.
#
# Output goes to /dev/console because login attached this shell to tty1, and
# every marker kde.sh prints has to reach the serial log to be checked there.
if [ -f /run/kde-session-attempted ] && [ -z "${KDE_SESSION_RUNNING:-}" ]; then
	rm -f /run/kde-session-attempted
	KDE_SESSION_RUNNING=1
	export KDE_SESSION_RUNNING
	exec /bin/sh /etc/kde.sh > /dev/console 2>&1
fi
