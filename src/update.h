#ifndef UPDATE_H_
#define UPDATE_H_

/* Updating isf from its releases on GitHub. This build knows which release it
 * came from (its version), which file of it to take (the architecture it was
 * built for, and whether it runs as an AppImage) and where that file is on
 * this machine (/proc/self/exe, or $APPIMAGE). The download needs curl or
 * wget: isf speaks no HTTPS itself. */

/* Is there a newer isf? Says so, and TAKE_IT (--update) takes it: downloads
 * it and puts it in the place of this one.
 * Exit status, for scripts:
 *   0  --check-update: this is the newest; --update: it is now (or already was)
 *   1  --check-update: a newer one has been released, and this isn't it
 *   2  it couldn't be done (logged): no network, no curl or wget, nowhere to
 *      write, or what came down wasn't isf */
int update_run(int take_it);

#endif // !UPDATE_H_
