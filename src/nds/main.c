#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nds_include.h"
#include <fat.h>
#include <filesystem.h>

#include "audio/data.h"
#include "audio/external.h"
#include "audio/internal.h"
#include "audio/load.h"
#include "audio/seqplayer.h"
#include "game/game_init.h"
#include "game/save_file.h"
#include "nds_renderer.h"
#include "nds_overlay.h"
#include "nds_sample_cache.h"
#include "nds_arm7_copy.h"
#include "nds_net.h"
#include "nds_netplay.h"
#include "nds_menu.h"


/*
 * ============================================================================
 *  SUPER MARIO 64 - Nintendo DS
 *  Avvio e gestione NitroFS
 * ============================================================================
 *
 * Obiettivi:
 *
 *  - Il gioco deve essere un singolo file .nds autosufficiente.
 *  - Nessuna dipendenza da cartelle hardcodate come /roms/nds, /nds o /roms.
 *  - Se il launcher fornisce argv[0], viene usato il percorso reale del gioco.
 *  - Se argv[0] manca o e' inutilizzabile, il programma cerca automaticamente
 *    la propria ROM sulla memoria FAT/SD.
 *  - La ROM viene riconosciuta confrontando il suo header NDS con quello della
 *    build attualmente in esecuzione.
 *  - NitroFS viene inizializzato soltanto dopo aver individuato il file corretto,
 *    evitando tentativi multipli su ROM sbagliate.
 *
 * Nota:
 * su una flashcard/R4 il kernel deve comunque fornire un driver DLDI/FAT
 * funzionante. Nessun programma DS puo' leggere un file dalla microSD se il
 * launcher non rende disponibile il filesystem.
 */


#define TBL_HEADER_SIZE          4096
#define NDS_ROM_PATH_MAX         512
#define NDS_SCAN_MAX_DEPTH       32
#define NDS_HEADER_PROBE_SIZE    0x170


u8 nds_audio_state;
static u8 audio_step;
static u8 fps;

extern unsigned char *gSoundDataADSR;
extern unsigned char *gSoundDataRaw;
extern unsigned char *gMusicData;

u8 gNdsAudioDisabled = 0;

/*
 * Stato di mute richiesto dal motore originale tramite set_audio_muted().
 * Non e' un mute del mixer DS: viene applicato selettivamente per Note.
 */
static volatile u8 sNdsSequenceMuted = 0;

#ifdef NDS_DEBUG_CONSOLE
static PrintConsole *sDebugConsole = NULL;
#define NDS_LOG(...) iprintf(__VA_ARGS__)
#else
#define NDS_LOG(...) ((void)0)
#endif


/*
 * Percorso del file .nds utilizzato per inizializzare NitroFS.
 *
 * Deve rimanere valido anche dopo il ritorno dalla funzione che prepara argv,
 * per questo viene conservato in memoria statica.
 */
static char sNdsRomPath[NDS_ROM_PATH_MAX];
static char *sNdsArgvPtr[1];


/* ------------------------------------------------------------------------- */
/* Console di debug                                                          */
/* ------------------------------------------------------------------------- */

void nds_debug_printf(const char *fmt, ...) {
#ifdef NDS_DEBUG_CONSOLE
    if (sDebugConsole && gNdsMenuOpen) {
        va_list args;
        va_start(args, fmt);
        consoleSelect(sDebugConsole);
        viprintf(fmt, args);
        va_end(args);
    }
#else
    (void)fmt;
#endif
}


void nds_debug_console_clear(void) {
#ifdef NDS_DEBUG_CONSOLE
    if (sDebugConsole) {
        consoleSelect(sDebugConsole);
        consoleClear();
    }
#endif
}


/* ------------------------------------------------------------------------- */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------- */

void exec_display_list(struct SPTask *spTask) {
    draw_frame((Gfx *)spTask->task.t.data_ptr);
    fps++;

#ifdef NDS_DEBUG_CONSOLE
    if (gNdsMenuOpen) {
        nds_menu_render();
    } else {
        nds_debug_console_clear();
    }
#endif

    nds_scache_update();
    nds_net_update();
}


/* ------------------------------------------------------------------------- */
/* Audio                                                                     */
/* ------------------------------------------------------------------------- */

void nds_audio_set_hardware_muted(u8 muted) {
    /*
     * external.c continua a chiamare questo hook quando SM64 entra/esce dal
     * mute. Qui salviamo solo lo stato: NON inviamo un mute globale all'ARM7.
     */
    sNdsSequenceMuted = muted ? 1 : 0;
}

static struct SequenceChannel *nds_note_get_channel(const struct Note *note) {
    struct SequenceChannelLayer *layer;

    if (note == NULL) {
        return NULL;
    }

    layer = note->parentLayer;
    if (layer == NULL || layer == NO_LAYER) {
        layer = note->prevParentLayer;
    }

    if (layer == NULL || layer == NO_LAYER) {
        return NULL;
    }

    return layer->seqChannel;
}

/*
 * Classifica una Note secondo il comportamento della pausa originale:
 *
 *   SEQ_PLAYER_LEVEL -> sempre muto   (BGM)
 *   SEQ_PLAYER_ENV   -> sempre muto   (musica/eventi ambiente)
 *   SEQ_PLAYER_SFX   -> dipende dal muteBehavior del SequenceChannel
 *
 * Nel sound player originale i canali continui ambientali usano un
 * muteBehavior non-zero (es. SOFTEN), mentre i suoni del menu pausa usano
 * muteBehavior == 0. In questo modo i primi vengono fermati e i secondi no.
 */
static bool nds_classify_note_pause_mute(const struct Note *note, u8 *outMute) {
    struct SequenceChannel *channel;
    struct SequencePlayer *player;

    if (outMute == NULL) {
        return false;
    }

    channel = nds_note_get_channel(note);
    if (channel == NULL) {
        return false;
    }

    player = channel->seqPlayer;
    if (player == NULL) {
        return false;
    }

    if (player == &gSequencePlayers[SEQ_PLAYER_LEVEL] ||
        player == &gSequencePlayers[SEQ_PLAYER_ENV]) {
        *outMute = 1;
        return true;
    }

    if (player == &gSequencePlayers[SEQ_PLAYER_SFX]) {
        *outMute = (channel->muteBehavior != 0);
        return true;
    }

    /* Fallback per eventuali player extra/versioni regionali. */
    *outMute = (channel->muteBehavior != 0);
    return true;
}

/*
 * Aggiorna/salva la classificazione PRIMA che process_sequences() possa
 * staccare parentLayer. La classificazione resta valida anche se durante la
 * pausa la Note perde il proprietario logico.
 */
static void nds_update_note_mute_flags(void) {
    int i;

    if (gNotes == NULL) {
        return;
    }

    for (i = 0; i < 20; i++) {
        struct Note *note = &gNotes[i];
        u8 pauseMute;

        if (!note->enabled) {
            note->ndsPauseMute = 0;
            note->ndsMuteSeq = note->ndsSeq;
            note->ndsMuteKnown = 0;
            note->ndsMuted = 0;
            continue;
        }

        /* Lo slot e' stato riusato per un nuovo sample: invalida la policy. */
        if (note->ndsMuteSeq != note->ndsSeq) {
            note->ndsPauseMute = 0;
            note->ndsMuteSeq = note->ndsSeq;
            note->ndsMuteKnown = 0;
        }

        if (nds_classify_note_pause_mute(note, &pauseMute)) {
            note->ndsPauseMute = pauseMute;
            note->ndsMuteSeq = note->ndsSeq;
            note->ndsMuteKnown = 1;
        }

        if (!sNdsSequenceMuted) {
            note->ndsMuted = 0;
        } else if (note->ndsMuteKnown) {
            note->ndsMuted = note->ndsPauseMute;
        } else {
            /*
             * Fail-safe contro il long note: se una vecchia Note in loop ha
             * perso il parent prima di essere classificata, la fermiamo.
             * Le nuove note del menu vengono riclassificate subito nel pass
             * post-process_sequences() e quindi tornano udibili se consentite.
             */
            note->ndsMuted = 1;
        }
    }
}

static void update_audio(void) {
    gNdsAudioTick++;

    if (gNdsAudioDisabled) {
        IPC_SendSync(0);
        return;
    }

    if (nds_audio_state == 0) {
        /*
         * Fondamentale: snapshot PRIMA del sequencer. Se la pausa stacca un
         * layer, sappiamo comunque quale SCHANNEL deve essere spento.
         */
        nds_update_note_mute_flags();

        /*
         * La logica audio viene aggiornata a frequenza ridotta rispetto
         * all'elaborazione delle sequenze.
         */
        if ((audio_step = (audio_step + 1) & 7) == 0) {
            update_game_sound();
            gAudioFrameCount += 2;
            gAudioRandom = ((gAudioRandom + gAudioFrameCount) * gAudioFrameCount);
        }

        process_sequences(0);
        nds_scache_retry_pending();

        /* Classifica anche le nuove note, compresi gli SFX del menu pausa. */
        nds_update_note_mute_flags();

    } else if (nds_audio_state == 1) {
        int i;

        for (i = 0; i < 16; i++) {
            gNotes[i].enabled = false;
        }

        nds_audio_state = 2;
    }

    if (gNotes != NULL) {
        nds_update_note_mute_flags();
        DC_FlushRange(gNotes, 20 * sizeof(struct Note));
    }

    IPC_SendSync(0);
}


static void update_fps(void) {
#ifdef NDS_DEBUG_CONSOLE
    if (sDebugConsole && !gNdsMenuOpen) {
        consoleSelect(sDebugConsole);
        consoleClear();
        NDS_LOG("FPS: %d\n", fps);
    }
#endif

    fps = 0;
}


/* ------------------------------------------------------------------------- */
/* Lettura dei file NitroFS                                                  */
/* ------------------------------------------------------------------------- */

static unsigned char *load_nitro_file(const char *path, unsigned long *outSize) {
    FILE *f;
    long size;
    unsigned char *buf;
    size_t readSize;

    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    buf = (unsigned char *)malloc((size_t)size);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }

    readSize = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if (readSize != (size_t)size) {
        free(buf);
        return NULL;
    }

    if (outSize != NULL) {
        *outSize = (unsigned long)size;
    }

    return buf;
}


static void nds_load_audio_data(void) {
    FILE *f;

    gSoundDataADSR = load_nitro_file(
        "nitro:/sound/sound_data.ctl",
        NULL
    );

    gMusicData = load_nitro_file(
        "nitro:/sound/sequences.bin",
        NULL
    );

    gSoundDataRaw = (unsigned char *)malloc(TBL_HEADER_SIZE);

    if (gSoundDataRaw == NULL) {
        return;
    }

    f = fopen("nitro:/sound/sound_data.tbl", "rb");

    if (f == NULL) {
        free(gSoundDataRaw);
        gSoundDataRaw = NULL;
        return;
    }

    if (fread(gSoundDataRaw, 1, TBL_HEADER_SIZE, f) != TBL_HEADER_SIZE) {
        free(gSoundDataRaw);
        gSoundDataRaw = NULL;
    }

    fclose(f);
}


static bool nds_assets_readable(void) {
    FILE *f;
    unsigned char probe[16];
    size_t got;

    f = fopen("nitro:/sound/sound_data.ctl", "rb");

    if (f == NULL) {
        return false;
    }

    got = fread(probe, 1, sizeof(probe), f);
    fclose(f);

    return got == sizeof(probe);
}


/* ------------------------------------------------------------------------- */
/* Gestione percorsi                                                         */
/* ------------------------------------------------------------------------- */

static bool nds_path_has_device(const char *path) {
    if (path == NULL) {
        return false;
    }

    return strncmp(path, "fat:", 4) == 0 ||
           strncmp(path, "sd:", 3) == 0;
}


static bool nds_has_nds_extension(const char *name) {
    size_t len;
    const char *ext;

    if (name == NULL) {
        return false;
    }

    len = strlen(name);

    if (len < 4) {
        return false;
    }

    ext = name + len - 4;

    return ext[0] == '.' &&
           ((ext[1] | 0x20) == 'n') &&
           ((ext[2] | 0x20) == 'd') &&
           ((ext[3] | 0x20) == 's');
}


static bool nds_join_path(
    char *out,
    size_t outSize,
    const char *base,
    const char *name
) {
    size_t baseLen;
    int written;

    if (out == NULL ||
        outSize == 0 ||
        base == NULL ||
        name == NULL) {
        return false;
    }

    baseLen = strlen(base);

    written = snprintf(
        out,
        outSize,
        "%s%s%s",
        base,
        (baseLen > 0 && base[baseLen - 1] == '/') ? "" : "/",
        name
    );

    return written >= 0 && (size_t)written < outSize;
}


/*
 * Converte un percorso eventualmente relativo in un percorso utilizzabile
 * da libfilesystem, senza eliminare nessuna parte del nome originale.
 *
 * Esempi:
 *
 *   fat:/Giochi/SM64/sm64.nds   -> invariato
 *   sd:/Homebrew/sm64.nds       -> invariato
 *   /Giochi/sm64.nds            -> fat:/Giochi/sm64.nds
 *   Giochi/sm64.nds             -> <cwd>/Giochi/sm64.nds
 *
 * Questo corregge il vecchio comportamento che usava strchr(path, '/')
 * e poteva perdere la prima directory del percorso.
 */
static bool nds_normalize_path(
    const char *path,
    char *out,
    size_t outSize
) {
    char cwd[NDS_ROM_PATH_MAX];
    int written;

    if (path == NULL || path[0] == '\0' || out == NULL || outSize == 0) {
        return false;
    }

    if (nds_path_has_device(path)) {
        written = snprintf(out, outSize, "%s", path);
        return written >= 0 && (size_t)written < outSize;
    }

    if (path[0] == '/') {
        /*
         * Se il launcher ha fornito un path assoluto senza device,
         * proviamo ad associarlo al device corrente.
         */
        if (getcwd(cwd, sizeof(cwd)) != NULL && nds_path_has_device(cwd)) {
            const char *colon = strchr(cwd, ':');

            if (colon != NULL) {
                size_t prefixLen = (size_t)(colon - cwd) + 1;

                if (prefixLen + strlen(path) + 1 <= outSize) {
                    memcpy(out, cwd, prefixLen);
                    out[prefixLen] = '\0';
                    strncat(out, path, outSize - strlen(out) - 1);
                    return true;
                }
            }
        }

        written = snprintf(out, outSize, "fat:%s", path);
        return written >= 0 && (size_t)written < outSize;
    }

    /*
     * Percorso relativo.
     *
     * Se getcwd() restituisce un percorso sul device corrente, manteniamo
     * l'intero percorso relativo. In caso contrario usiamo fat:/ come
     * convenzione standard del filesystem inizializzato da fatInitDefault().
     */
    if (getcwd(cwd, sizeof(cwd)) != NULL && cwd[0] != '\0') {
        if (nds_path_has_device(cwd)) {
            return nds_join_path(out, outSize, cwd, path);
        }

        if (cwd[0] == '/') {
            char temp[NDS_ROM_PATH_MAX];

            if (!nds_join_path(temp, sizeof(temp), cwd, path)) {
                return false;
            }

            written = snprintf(out, outSize, "fat:%s", temp);
            return written >= 0 && (size_t)written < outSize;
        }
    }

    written = snprintf(out, outSize, "fat:/%s", path);
    return written >= 0 && (size_t)written < outSize;
}


/* ------------------------------------------------------------------------- */
/* Identificazione della ROM in esecuzione                                   */
/* ------------------------------------------------------------------------- */

/*
 * Confronta l'header di un file .nds con l'header della build in memoria.
 *
 * Non ci limitiamo al nome del file: il gioco puo' essere rinominato liberamente.
 * Il confronto usa:
 *
 *  - titolo + game code (primi 16 byte)
 *  - configurazione ARM9
 *  - configurazione ARM7
 *  - FNT/FAT NitroFS
 *  - CRC dell'header
 *
 * Questo rende estremamente improbabile selezionare per errore un altro .nds.
 */
static bool nds_header_matches_self(const u8 *candidate, size_t size) {
    const u8 *self = (const u8 *)__NDSHeader;

    if (candidate == NULL || size < 0x160) {
        return false;
    }

    if (memcmp(candidate + 0x00, self + 0x00, 0x10) != 0) {
        return false;
    }

    if (memcmp(candidate + 0x20, self + 0x20, 0x20) != 0) {
        return false;
    }

    if (memcmp(candidate + 0x40, self + 0x40, 0x10) != 0) {
        return false;
    }

    if (memcmp(candidate + 0x15E, self + 0x15E, 2) != 0) {
        return false;
    }

    return true;
}


static bool nds_file_is_self(const char *path) {
    FILE *f;
    u8 header[NDS_HEADER_PROBE_SIZE];
    size_t got;

    if (path == NULL || !nds_has_nds_extension(path)) {
        return false;
    }

    f = fopen(path, "rb");

    if (f == NULL) {
        return false;
    }

    got = fread(header, 1, sizeof(header), f);
    fclose(f);

    return nds_header_matches_self(header, got);
}


/* ------------------------------------------------------------------------- */
/* Ricerca automatica della ROM                                              */
/* ------------------------------------------------------------------------- */

/*
 * Cerca ricorsivamente la build corrente.
 *
 * Non esistono piu' cartelle privilegiate:
 * il file puo' trovarsi in qualunque directory accessibile tramite FAT/SD.
 */
static bool nds_find_self_recursive(
    const char *directory,
    char *result,
    size_t resultSize,
    int depth
) {
    DIR *dir;
    struct dirent *entry;

    if (directory == NULL ||
        result == NULL ||
        resultSize == 0 ||
        depth > NDS_SCAN_MAX_DEPTH) {
        return false;
    }

    dir = opendir(directory);

    if (dir == NULL) {
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[NDS_ROM_PATH_MAX];
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (!nds_join_path(path, sizeof(path), directory, entry->d_name)) {
            continue;
        }

        if (stat(path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (nds_find_self_recursive(
                    path,
                    result,
                    resultSize,
                    depth + 1)) {
                closedir(dir);
                return true;
            }

            continue;
        }

        if (!nds_has_nds_extension(entry->d_name)) {
            continue;
        }

        if (nds_file_is_self(path)) {
            strncpy(result, path, resultSize - 1);
            result[resultSize - 1] = '\0';

            closedir(dir);
            return true;
        }
    }

    closedir(dir);
    return false;
}


/*
 * Prova prima la directory corrente.
 *
 * Molti launcher homebrew impostano correttamente il working directory anche
 * quando non forniscono un argv completo. Questo permette di trovare subito
 * la ROM senza scandire tutta la scheda.
 */
static bool nds_find_self_from_cwd(
    char *result,
    size_t resultSize
) {
    char cwd[NDS_ROM_PATH_MAX];

    if (getcwd(cwd, sizeof(cwd)) == NULL || cwd[0] == '\0') {
        return false;
    }

    NDS_LOG("[Ricerca] Directory corrente:\n%s\n", cwd);

    return nds_find_self_recursive(
        cwd,
        result,
        resultSize,
        0
    );
}


/*
 * Fallback universale.
 *
 * Vengono provati i nomi di device normalmente utilizzati da libfat/libfilesystem,
 * NON directory di giochi.
 *
 * "fat:/" e "sd:/" rappresentano il filesystem, non una posizione obbligatoria
 * della ROM.
 */
static bool nds_find_self_anywhere(
    char *result,
    size_t resultSize
) {
    static const char *roots[] = {
        "fat:/",
        "sd:/",
        NULL
    };

    int i;

    /*
     * Prima la directory corrente: e' il caso piu' veloce.
     */
    if (nds_find_self_from_cwd(result, resultSize)) {
        return true;
    }

    /*
     * Se non basta, cerchiamo sull'intero filesystem disponibile.
     */
    for (i = 0; roots[i] != NULL; i++) {
        NDS_LOG("[Ricerca] Scansione %s\n", roots[i]);

        if (nds_find_self_recursive(
                roots[i],
                result,
                resultSize,
                0)) {
            return true;
        }
    }

    return false;
}


/* ------------------------------------------------------------------------- */
/* Mount NitroFS                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Prepara __system_argv in modo che nitroFSInit() apra esattamente
 * il file .nds individuato.
 */
static bool nds_prepare_argv_for_rom(const char *path) {
    struct __argv *av = __system_argv;
    size_t len;

    if (path == NULL || path[0] == '\0') {
        return false;
    }

    len = strlen(path);

    if (len + 1 > sizeof(sNdsRomPath)) {
        return false;
    }

    memcpy(sNdsRomPath, path, len + 1);

    sNdsArgvPtr[0] = sNdsRomPath;

    av->argvMagic = ARGV_MAGIC;
    av->commandLine = sNdsRomPath;
    av->length = len + 1;
    av->argc = 1;
    av->argv = sNdsArgvPtr;

    return true;
}


static bool nds_mount_nitrofs_from_rom(const char *path) {
    if (!nds_prepare_argv_for_rom(path)) {
        return false;
    }

    NDS_LOG("[NitroFS] ROM individuata:\n%s\n", sNdsRomPath);
    NDS_LOG("[NitroFS] Montaggio...\n");

    if (!nitroFSInit(NULL)) {
        NDS_LOG("[NitroFS] Montaggio fallito.\n");
        return false;
    }

    if (!nds_assets_readable()) {
        NDS_LOG("[NitroFS] File dati non leggibili.\n");
        return false;
    }

    NDS_LOG("[NitroFS] Pronto.\n");
    return true;
}


/*
 * Modalita' diretta per cartuccia reale/emulatori/loader che espongono
 * direttamente il bus della ROM.
 *
 * Questa strada viene usata soltanto se non e' stato possibile individuare
 * un file .nds tramite FAT/SD.
 */
static bool nds_try_direct_nitrofs(void) {
    NDS_LOG("[NitroFS] Provo accesso diretto...\n");

    if (!nitroFSInit(NULL)) {
        NDS_LOG("[NitroFS] Accesso diretto fallito.\n");
        return false;
    }

    if (!nds_assets_readable()) {
        NDS_LOG("[NitroFS] Dati non disponibili.\n");
        return false;
    }

    NDS_LOG("[NitroFS] Accesso diretto riuscito.\n");
    return true;
}


/* ------------------------------------------------------------------------- */
/* Inizializzazione storage                                                  */
/* ------------------------------------------------------------------------- */

static bool nds_storage_init(void) {
    struct __argv *av = __system_argv;
    bool fatOk;
    bool argvOk;
    const char *argvPath;
    char normalized[NDS_ROM_PATH_MAX];
    char found[NDS_ROM_PATH_MAX];

    NDS_LOG("Modalita': %s\n", isDSiMode() ? "DSi" : "DS");

    fatOk = fatInitDefault();
    NDS_LOG("Filesystem FAT: %s\n", fatOk ? "disponibile" : "non disponibile");

    argvOk =
        av->argvMagic == ARGV_MAGIC &&
        av->argc >= 1 &&
        av->argv != NULL &&
        av->argv[0] != NULL &&
        av->argv[0][0] != '\0';

    argvPath = argvOk ? av->argv[0] : NULL;

    if (argvPath != NULL) {
        NDS_LOG("[Avvio] Percorso launcher:\n%s\n", argvPath);

        if (fatOk &&
            nds_normalize_path(
                argvPath,
                normalized,
                sizeof(normalized))) {

            NDS_LOG("[Avvio] Percorso normalizzato:\n%s\n", normalized);

            if (nds_file_is_self(normalized)) {
                NDS_LOG("[Avvio] ROM verificata.\n");
                return nds_mount_nitrofs_from_rom(normalized);
            }

            NDS_LOG("[Avvio] Il percorso non identifica questa ROM.\n");
        }
    } else {
        NDS_LOG("[Avvio] Il launcher non ha fornito argv[0].\n");
    }

    /*
     * Nessun percorso hardcodato.
     *
     * Se FAT e' disponibile cerchiamo la ROM ovunque si trovi.
     */
    if (fatOk) {
        NDS_LOG("[Ricerca] Individuazione automatica della ROM...\n");

        if (nds_find_self_anywhere(found, sizeof(found))) {
            NDS_LOG("[Ricerca] ROM trovata.\n");
            return nds_mount_nitrofs_from_rom(found);
        }

        NDS_LOG("[Ricerca] ROM non trovata sul filesystem.\n");
    }

    /*
     * Ultimo tentativo:
     * utile su cartuccia reale o emulatori con accesso diretto alla ROM.
     */
    return nds_try_direct_nitrofs();
}


/* ------------------------------------------------------------------------- */
/* Schermata di errore                                                       */
/* ------------------------------------------------------------------------- */

static void nds_fatal_screen(const char *what) {
    PrintConsole *con;

    videoSetModeSub(MODE_0_2D);
    vramSetBankH(VRAM_H_SUB_BG);

    con = consoleInit(
        NULL,
        0,
        BgType_Text4bpp,
        BgSize_T_256x256,
        15,
        0,
        false,
        true
    );

    consoleSelect(con);
    consoleClear();

    iprintf(" SUPER MARIO 64 DS\n");
    iprintf(" -----------------\n\n");

    iprintf(" ERRORE DI AVVIO\n\n");

    iprintf(" %s\n\n", what);

    iprintf("Il gioco non riesce a leggere\n");
    iprintf("i dati interni della ROM.\n\n");

    iprintf("La posizione del file .nds NON\n");
    iprintf("e' importante.\n\n");

    iprintf("Su flashcard/R4 e' necessario\n");
    iprintf("un driver DLDI/FAT funzionante.\n\n");

    iprintf("Su DSi/3DS usare un loader che\n");
    iprintf("supporti correttamente homebrew\n");
    iprintf("e NitroFS.\n");

    while (1) {
        swiWaitForVBlank();
    }
}


/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

int main(void) {
    const size_t poolSize = 0x158000;
    u8 *pool;

#ifdef NDS_DEBUG_CONSOLE
    /*
     * Console diagnostica visibile solo nelle build debug.
     * Nelle build release non viene inizializzata alcuna console di avvio.
     */
    videoSetModeSub(MODE_0_2D);
    vramSetBankH(VRAM_H_SUB_BG);

    sDebugConsole = consoleInit(
        NULL,
        0,
        BgType_Text4bpp,
        BgSize_T_256x256,
        15,
        0,
        false,
        true
    );

    NDS_LOG("SUPER MARIO 64 DS\n");
    NDS_LOG("Console di avvio\n");
    NDS_LOG("=================\n\n");
#endif

    /*
     * Pool principale.
     */
    NDS_LOG("Inizializzazione memoria...\n");

    pool = (u8 *)malloc(poolSize);

    if (pool == NULL) {
        nds_fatal_screen("Memoria principale insufficiente.");
    }

    main_pool_init(pool, pool + poolSize);
    gEffectsMemoryPool = mem_pool_init(0x4000, MEMORY_POOL_LEFT);

    NDS_LOG(
        "Pool: %p - dimensione 0x%lx\n",
        (void *)pool,
        (unsigned long)poolSize
    );

    /*
     * Storage e NitroFS.
     */
    NDS_LOG("\nInizializzazione archivio...\n");

    if (!nds_storage_init()) {
        nds_fatal_screen("Impossibile leggere i dati della ROM.");
    }

    /*
     * Renderer.
     */
    NDS_LOG("\nInizializzazione grafica...\n");

    renderer_init();

    NDS_LOG("Grafica pronta.\n");

#ifdef NDS_DEBUG_CONSOLE
    /*
     * renderer_init() puo' riconfigurare il sottoschermo.
     * In debug ricreiamo quindi la console dopo il renderer.
     */
    vramSetBankH(VRAM_H_SUB_BG);

    sDebugConsole = consoleInit(
        NULL,
        0,
        BgType_Text4bpp,
        BgSize_T_256x256,
        15,
        0,
        false,
        true
    );
#endif

    /*
     * VRAM audio ARM7.
     */
    vramSetBankC(VRAM_C_ARM7_0x06000000);
    vramSetBankD(VRAM_D_ARM7_0x06020000);

    /*
     * Audio.
     */
    NDS_LOG("\nCaricamento dati audio...\n");

    nds_load_audio_data();

    NDS_LOG(
        "ADSR=%p RAW=%p SEQ=%p\n",
        (void *)gSoundDataADSR,
        (void *)gSoundDataRaw,
        (void *)gMusicData
    );

    if (!gSoundDataADSR ||
        !gSoundDataRaw ||
        !gMusicData) {

        NDS_LOG("Dati audio mancanti: audio disattivato.\n");
        gNdsAudioDisabled = 1;

    } else if (nds_scache_init() != 0) {

        NDS_LOG("Cache campioni non disponibile: audio disattivato.\n");
        gNdsAudioDisabled = 1;

    } else {

        nds_scache_add_region(
            NDS_VRAMC_SAMPLE_BASE,
            NDS_VRAM_BANK_SIZE
        );

        nds_scache_add_region(
            NDS_VRAMD_SAMPLE_BASE,
            NDS_VRAM_BANK_SIZE
        );
    }

    if (!gNdsAudioDisabled) {
        NDS_LOG("Inizializzazione audio...\n");
        audio_init();

        NDS_LOG("Inizializzazione effetti sonori...\n");
        sound_init();

        NDS_LOG("Audio pronto.\n");
    } else {
        NDS_LOG("Inizializzazione audio ignorata.\n");
    }

    /*
     * Sincronizzazione audio ARM9 -> ARM7.
     */
    irqSet(IRQ_IPC_SYNC, update_audio);
    irqEnable(IRQ_IPC_SYNC);

    fifoSendValue32(FIFO_USER_01, (u32)gNotes);

#ifdef ENABLE_FPS
    timerStart(
        0,
        ClockDivider_1024,
        TIMER_FREQ_1024(1),
        update_fps
    );
#endif

    /*
     * Multiplayer / netplay.
     */
    NDS_LOG("\nInizializzazione multiplayer...\n");
    nds_netplay_init();

    /*
     * Avvio del gioco.
     */
    NDS_LOG("Avvio del gioco.\n");

    thread5_game_loop(NULL);

    return 0;
}
