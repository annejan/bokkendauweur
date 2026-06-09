#ifndef GFILE_H
#define GFILE_H

#define MAX_DIRFILES 16384
// Bumped from 60 to 512 so absolute Linux/Mac paths (which routinely run
// 80-120 chars between /home/<user> + /work/<project>/examples/...) fit
// without truncation. The original 60-byte cap was inherited from the
// DOS-era SDL build and silently dropped the tail of long paths inside
// loadsong / mergesong, making files appear empty on load.
#define MAX_FILENAME 512
#define MAX_PATHNAME 512

typedef struct
{
  char *name;
  int attribute;
} DIRENTRY;

void initpaths(void);
int fileselector(char *name, char *path, char *filter, char *title, int filemode);
void editstring(char *buffer, int maxlength);
int cmpname(char *string1, char *string2);

#endif

