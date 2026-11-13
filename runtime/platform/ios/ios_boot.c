/*
 * iOS start-up (TARGET=ios, runtime/build.py): the game on an iPhone/iPad has no command line and no environment, so this
 * finds the game's files -- the folder "GE2RB" the player copied beside the app -- and gives main() what `vitarecomp run`
 * (or an AppRun, pipeline/appimage.py) would: the working directory, the environment, and the arguments
 *   <image.img> <module_start hex> <budget>.
 *
 * The folder holds ge2rb.cfg (written by dev/ios/stage_gamedata.py): KEY=VALUE lines, # comments.
 *   IMAGE=game.img        the game's memory image, in the folder   ENTRY=<hex>   module_start   BUDGET=<n>   import-call budget
 *   any other KEY         set in the environment as given; "@DATA@" in a value is this folder's path
 * Looked for, in order: <app's Documents>/GE2RB, <app's Documents>, /var/mobile/Documents/GE2RB, /var/mobile/Media/GE2RB (the
 * last two for an app installed with TrollStore's no-sandbox entitlement). The game's stderr goes to session.log there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <SDL3/SDL_messagebox.h>

int ios_boot(int *argc, char ***argv);

static int has_cfg(const char *dir) {
	char p[1100]; snprintf(p, sizeof p, "%s/ge2rb.cfg", dir);
	return access(p, R_OK) == 0;
}

static void say_missing(const char *docs) {
	char dir[1024], p[1100];
	snprintf(dir, sizeof dir, "%s/GE2RB", docs);
	mkdir(docs, 0755); mkdir(dir, 0755);   /* (so the app's folder shows up in Files: the player knows where the game's folder goes) */
	snprintf(p, sizeof p, "%s/README.txt", dir);
	FILE *f = fopen(p, "w");
	const char *text =
		"The game's files are not here yet.\n"
		"Copy the contents of the gamedata download (ge2rb.cfg, game.img, fs/ ...) into this folder, so that\n"
		"GE2RB/ge2rb.cfg exists, then start the app again.\n";
	if (f) { fputs(text, f); fclose(f); }
	fprintf(stderr, "ios_boot: no ge2rb.cfg found (looked in %s/GE2RB, %s, /var/mobile/Documents/GE2RB, /var/mobile/Media/GE2RB)\n", docs, docs);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game files not found",
		"Copy the gamedata folder (it holds ge2rb.cfg) into this app's folder in the Files app (On My iPhone > GE2RB), then start the app again.", NULL);
}

static void put(char *dst, size_t n, const char *s, const char *data) {   /* s with "@DATA@" replaced by data */
	size_t o = 0; size_t dl = strlen(data);
	while (*s && o + 1 < n) {
		if (!strncmp(s, "@DATA@", 6)) { for (size_t i = 0; i < dl && o + 1 < n; i++) dst[o++] = data[i]; s += 6; }
		else dst[o++] = *s++;
	}
	dst[o] = 0;
}

int ios_boot(int *argc, char ***argv) {
	static char *av[5]; static char image[1200], entry[64], budget[64];
	const char *home = getenv("HOME");
	char docs[1024], cand[4][1100], data[1100] = "";
	snprintf(docs, sizeof docs, "%s/Documents", home && *home ? home : "/var/mobile");
	snprintf(cand[0], sizeof cand[0], "%s/GE2RB", docs);
	snprintf(cand[1], sizeof cand[1], "%s", docs);
	snprintf(cand[2], sizeof cand[2], "/var/mobile/Documents/GE2RB");
	snprintf(cand[3], sizeof cand[3], "/var/mobile/Media/GE2RB");
	for (int i = 0; i < 4 && !data[0]; i++) if (has_cfg(cand[i])) snprintf(data, sizeof data, "%s", cand[i]);
	if (!data[0]) { say_missing(docs); return 1; }
	if (chdir(data)) { fprintf(stderr, "ios_boot: cannot enter %s\n", data); return 1; }
	{	/* the log of this start (the one before it kept): what the player sends when the game doesn't run */
		rename("session.log", "session.prev.log");
		if (!freopen("session.log", "w", stderr)) { /* (stderr stays where it was) */ }
		setvbuf(stderr, NULL, _IOLBF, 0);
	}
	mkdir("captures", 0755); mkdir("fs", 0755);
	setenv("RECOMP_OSD", "2", 0);   /* the stats overlay (fps, CPU, memory, GPU, shader builds) on from the start: a phone has no F1 key; ge2rb.cfg RECOMP_OSD=0 turns it off */
	snprintf(image, sizeof image, "%s/game.img", data); snprintf(entry, sizeof entry, "%s", "0"); snprintf(budget, sizeof budget, "%s", "100000000000");
	FILE *f = fopen("ge2rb.cfg", "r");
	char *line = NULL; size_t cap = 0; ssize_t len;
	while (f && (len = getline(&line, &cap, f)) > 0) {
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
		if (!*line || *line == '#') continue;
		char *eq = strchr(line, '='); if (!eq || eq == line) continue;
		*eq = 0; const char *k = line, *v = eq + 1;
		size_t nd = 0; for (const char *q = v; (q = strstr(q, "@DATA@")); q += 6) nd++;   /* (every @DATA@ becomes the folder: RECOMP_LOAD_MODULES has one per module) */
		size_t vn = strlen(v) + nd * strlen(data) + 16; char *val = malloc(vn); if (!val) continue;
		put(val, vn, v, data);
		if (!strcmp(k, "IMAGE")) snprintf(image, sizeof image, "%s/%s", data, val);
		else if (!strcmp(k, "ENTRY")) snprintf(entry, sizeof entry, "%s", val);
		else if (!strcmp(k, "BUDGET")) snprintf(budget, sizeof budget, "%s", val);
		else setenv(k, val, 1);
		free(val);
	}
	free(line); if (f) fclose(f);
	if (!getenv("RECOMP_FS")) { char fs[1200]; snprintf(fs, sizeof fs, "%s/fs", data); setenv("RECOMP_FS", fs, 1); }
	fprintf(stderr, "ios_boot: game folder %s, image %s, entry %s\n", data, image, entry);
	av[0] = (*argv && (*argv)[0]) ? (*argv)[0] : "ge2rb"; av[1] = image; av[2] = entry; av[3] = budget; av[4] = NULL;
	*argc = 4; *argv = av;
	return 0;
}
