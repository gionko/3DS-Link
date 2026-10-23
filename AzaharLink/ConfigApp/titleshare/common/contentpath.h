#pragma once
#include <stddef.h>
#include <stdint.h>

// Posizione dei file .app di un titolo installato: direttamente in content/ per i giochi, in una sottocartella
// numerata di content/ (di solito 00000000/) per i DLC. Le sottocartelle si scoprono leggendo la cartella.

#define CONTENT_MAX_SUBDIRS 16

typedef struct {
    int n;
    char sub[CONTENT_MAX_SUBDIRS][16];
    int last; // sottocartella dell'ultimo content trovato (-1 = nessuna), si prova per prima
} content_dirs;

// Elenca le sottocartelle di `contentDir` (esclusa "cmd").
void content_dirs_scan(content_dirs* d, const char* contentDir);

// Cerca il file <id>.app. Su successo ritorna 0 e scrive il percorso reale in `path`, quello relativo a ID1 in `rel`
// (serve al contatore AES dello strato SD) e in `*subIdx` l'indice della sottocartella (-1 = content/ stessa).
int content_locate(content_dirs* d, const char* contentDir, const char* contentRel, uint32_t id, char* path, size_t pathSize,
                   char* rel, size_t relSize, int* subIdx, uint64_t* fileSize);

// Ricostruisce i percorsi di un content di cui si conosce gia' la sottocartella.
void content_paths(const content_dirs* d, const char* contentDir, const char* contentRel, uint32_t id, int subIdx, char* path,
                   size_t pathSize, char* rel, size_t relSize);
