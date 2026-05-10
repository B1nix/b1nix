#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <b1nix/syscall.h>

/*
 * nmake — Minimal Make Utility
 * Parses simple Makefile rules and builds targets.
 */

/* ── Data structures ── */

#define MAX_RULES 64
#define MAX_DEPS  32

struct rule {
	char target[64];
	char deps[MAX_DEPS][64];
	int  dep_count;
	char commands[MAX_DEPS][128];
	int  cmd_count;
};

static struct rule rules[MAX_RULES];
static int rule_count = 0;

/* ── File existence check ── */

static int file_exists(const char *path)
{
	u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)path, 0, 0, 0, 0, 0);
	if (fd != (u64)-1) {
		syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
		return 1;
	}
	return 0;
}

/* ── Simple Makefile parser ── */

static void trim(char *s)
{
	/* Remove leading spaces */
	char *start = s;
	while (*start == ' ' || *start == '\t') start++;
	if (start != s) memmove(s, start, strlen(start) + 1);
	
	/* Remove trailing spaces / newline */
	int len = strlen(s);
	while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\n' || s[len-1] == '\r')) {
		s[--len] = '\0';
	}
}

static int parse_makefile(const char *content)
{
	rule_count = 0;
	
	char *buf = strdup(content);  /* We'll use kmalloc via strdup approximation */
	if (!buf) return -1;
	
	char *line = strtok(buf, "\n");
	struct rule *current_rule = 0;
	
	while (line) {
		trim(line);
		
		/* Skip comments and empty lines */
		if (line[0] == '#' || line[0] == '\0') {
			line = strtok(0, "\n");
			continue;
		}
		
		/* Check if this is a rule line (contains ':') */
		char *colon = strchr(line, ':');
		if (colon) {
			if (rule_count >= MAX_RULES) break;
			
			current_rule = &rules[rule_count++];
			memset(current_rule, 0, sizeof(struct rule));
			
			/* Target */
			*colon = '\0';
			trim(line);
			strcpy(current_rule->target, line);
			
			/* Dependencies */
			char *dep = colon + 1;
			current_rule->dep_count = 0;
			char *token = strtok(dep, " ");
			while (token && current_rule->dep_count < MAX_DEPS) {
				trim(token);
				if (token[0] != '\0') {
					strcpy(current_rule->deps[current_rule->dep_count], token);
					current_rule->dep_count++;
				}
				token = strtok(0, " ");
			}
			
			current_rule->cmd_count = 0;
		} else if (line[0] == '\t' && current_rule) {
			/* Command line */
			trim(line);
			if (line[0] && current_rule->cmd_count < MAX_DEPS) {
				strcpy(current_rule->commands[current_rule->cmd_count], line);
				current_rule->cmd_count++;
			}
		}
		
		line = strtok(0, "\n");
	}
	
	free(buf);
	return rule_count;
}

/* ── Build engine ── */

static void run_command(const char *cmd)
{
	printf("  %s\n", cmd);
	
	/* Parse command into args */
	char cmd_buf[256];
	strcpy(cmd_buf, cmd);
	char *args[16];
	int argc = 0;
	
	char *token = strtok(cmd_buf, " ");
	while (token && argc < 16) {
		args[argc++] = token;
		token = strtok(0, " ");
	}
	
	if (argc == 0) return;
	
	/* Try to find program */
	u64 pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)args[0], argc, (u64)(usize)args, 0, 0, 0);
	if (pid == (u64)-1) {
		char bin_path[128];
		bin_path[0] = '/';
		bin_path[1] = 'b';
		bin_path[2] = 'i';
		bin_path[3] = 'n';
		bin_path[4] = '/';
		strcpy(bin_path + 5, args[0]);
		pid = syscall_dispatch(SYS_SPAWN, (u64)(usize)bin_path, argc, (u64)(usize)args, 0, 0, 0);
	}
	
	if (pid != (u64)-1) {
		int status;
		syscall_dispatch(SYS_WAIT, pid, (u64)(usize)&status, 0, 0, 0, 0);
	} else {
		printf("nmake: %s: command not found\n", args[0]);
	}
}

static int needs_build(struct rule *r)
{
	/* If target doesn't exist, needs build */
	if (!file_exists(r->target)) return 1;
	
	/* If any dependency is newer (doesn't exist or target is missing) */
	/* For simplicity, if all deps exist, skip; otherwise build */
	for (int i = 0; i < r->dep_count; i++) {
		if (!file_exists(r->deps[i])) return 1;
	}
	
	/* In a real make, we'd compare timestamps. For now just always build. */
	return 1;
}

static int build_target(const char *target);

static int build_target_recursive(const char *target, int depth)
{
	if (depth > 20) {
		printf("nmake: Circular dependency detected?\n");
		return -1;
	}
	
	/* Find the rule */
	struct rule *r = 0;
	for (int i = 0; i < rule_count; i++) {
		if (strcmp(rules[i].target, target) == 0) {
			r = &rules[i];
			break;
		}
	}
	
	if (!r) {
		/* Check if file exists (leaf target) */
		if (file_exists(target)) return 0;
		printf("nmake: No rule to make target '%s'\n", target);
		return -1;
	}
	
	/* Build dependencies first */
	for (int i = 0; i < r->dep_count; i++) {
		if (build_target_recursive(r->deps[i], depth + 1) < 0) {
			return -1;
		}
	}
	
	/* Check if target needs building */
	if (!needs_build(r)) {
		printf("nmake: '%s' is up to date.\n", target);
		return 0;
	}
	
	printf("nmake: Building '%s'...\n", target);
	
	/* Run commands */
	for (int i = 0; i < r->cmd_count; i++) {
		run_command(r->commands[i]);
	}
	
	return 0;
}

static int build_target(const char *target)
{
	return build_target_recursive(target, 0);
}

/* ── Load default Makefile ── */

static int load_default_makefile(void)
{
	/* Try to read Makefile, makefile, or default content */
	u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)"Makefile", 0, 0, 0, 0, 0);
	char path[64] = "Makefile";
	
	if (fd == (u64)-1) {
		fd = syscall_dispatch(SYS_OPEN, (u64)(usize)"makefile", 0, 0, 0, 0, 0);
		strcpy(path, "makefile");
	}
	
	if (fd == (u64)-1) {
		/* No Makefile found */
		return -1;
	}
	
	char content[4096];
	int pos = 0;
	char buf[256];
	
	while (1) {
		u64 n = syscall_dispatch(SYS_READ, fd, (u64)(usize)buf, 255, 0, 0, 0);
		if (n == 0 || n == (u64)-1) break;
		buf[n] = '\0';
		int copy_len = strlen(buf);
		if (pos + copy_len < (int)sizeof(content) - 1) {
			memcpy(content + pos, buf, copy_len);
			pos += copy_len;
		}
	}
	
	syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
	content[pos] = '\0';
	
	return parse_makefile(content);
}

/* ── Main ── */

int nmake_main(int argc, const char **argv)
{
	const char *target = 0;
	
	/* Parse args */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
			i++;
			/* Manual makefile path — read it */
			u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)argv[i], 0, 0, 0, 0, 0);
			if (fd == (u64)-1) {
				printf("nmake: %s: No such file\n", argv[i]);
				return 1;
			}
			char content[4096];
			int pos = 0;
			char buf[256];
			while (1) {
				u64 n = syscall_dispatch(SYS_READ, fd, (u64)(usize)buf, 255, 0, 0, 0);
				if (n == 0 || n == (u64)-1) break;
				buf[n] = '\0';
				int copy_len = strlen(buf);
				if (pos + copy_len < (int)sizeof(content) - 1) {
					memcpy(content + pos, buf, copy_len);
					pos += copy_len;
				}
			}
			syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
			content[pos] = '\0';
			parse_makefile(content);
		} else if (argv[i][0] != '-') {
			target = argv[i];
		}
	}
	
	/* Try loading default Makefile if no -f given */
	if (rule_count == 0) {
		if (load_default_makefile() < 0) {
			printf("nmake: No Makefile found. Use 'nmake -f <file>' or create a Makefile.\n");
			return 1;
		}
	}
	
	if (rule_count == 0) {
		printf("nmake: No targets defined in Makefile.\n");
		return 1;
	}
	
	/* Use first target by default */
	if (!target) target = rules[0].target;
	
	return build_target(target);
}
