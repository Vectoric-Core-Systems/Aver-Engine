/* The machine-readable lines a command prints on stdout, so a tool that runs it (the editor's import dialog, a script, CI) can show its
 * progress and read its outcome without knowing the command (docs/EXTENSIONS.md, "The command protocol").
 *
 * One JSON object per line, UTF-8, ending in '\n', with nothing else on the line. Every other line the command prints is for people.
 * A line this version does not know is ignored, and so is a key it does not know.
 *
 *   {"aver":"progress","fraction":0.4,"text":"scenes"}
 *       fraction in [0, 1], or -1 (or absent) when unknown; text is optional.
 *
 *   {"aver":"result","ok":true,"open":"C:/out/Game/Game.ocproject","report":"C:/out/Report.md","warnings":3,"errors":0,"message":"71 converted"}
 *       The command's outcome, printed once, last. ok: it did what was asked; open: the file the tool may offer to open ("" for none);
 *       report: a file with the details ("" for none); warnings, errors: counts the tool shows; message: one line for a person, the
 *       failure's reason when !ok. A command that exits non-zero, or prints no result line, has failed.
 */
#ifndef AVER_EXTENSIONS_AVER_EXT_PROTOCOL_H
#define AVER_EXTENSIONS_AVER_EXT_PROTOCOL_H

#define AVER_EXT_MSG_KEY       "aver"
#define AVER_EXT_MSG_PROGRESS  "progress"
#define AVER_EXT_MSG_RESULT    "result"

/* The version of the protocol above. A new key or message kind keeps it; a change to what an existing one means raises it. */
#define AVER_EXT_PROTOCOL 1

#endif
