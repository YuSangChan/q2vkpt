/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

//
// cl_download.c -- queue manager and UDP downloads
//

#include "cl_local.h"
#include "d_md2.h"
#include "d_sp2.h"

typedef enum {
    PRECACHE_MODELS,
    PRECACHE_OTHER,
    PRECACHE_MAP,
    PRECACHE_FINAL
} precache_t;

static precache_t precache_check;
static int precache_sexed_sounds[MAX_SOUNDS];
static int precache_sexed_total;

/*
===============
CL_QueueDownload

Adds new download path into queue, incrementing pending count.
Entry will stay in queue for entire lifetime of server connection,
to make sure each path is tried exactly once.
===============
*/
qerror_t CL_QueueDownload(const char *path, dltype_t type)
{
    dlqueue_t *q;
    size_t len;

    FOR_EACH_DLQ(q) {
        // avoid sending duplicate requests
        if (!FS_pathcmp(path, q->path)) {
            Com_DPrintf("%s: %s [DUP]\n", __func__, path);
            return Q_ERR_EXIST;
        }
    }

    len = strlen(path);
    if (len >= MAX_QPATH) {
        Com_Error(ERR_DROP, "%s: oversize quake path", __func__);
    }

    q = Z_Malloc(sizeof(*q) + len);
    memcpy(q->path, path, len + 1);
    q->type = type;
    q->state = DL_PENDING;

#if USE_CURL
    // paks get bumped to the top and HTTP switches to single downloading.
    // this prevents someone on 28k dialup trying to do both the main .pak
    // and referenced configstrings data at once.
    if (type == DL_PAK)
        List_Insert(&cls.download.queue, &q->entry);
    else
#endif
        List_Append(&cls.download.queue, &q->entry);

    cls.download.pending++;
    Com_DPrintf("%s: %s [%d]\n", __func__, path, cls.download.pending);
    return Q_ERR_SUCCESS;
}

/*
===============
CL_FinishDownload

Mark the queue entry as done, decrementing pending count.
===============
*/
void CL_FinishDownload(dlqueue_t *q)
{
    if (q->state == DL_DONE) {
        Com_Error(ERR_DROP, "%s: already done", __func__);
    }
    if (!cls.download.pending) {
        Com_Error(ERR_DROP, "%s: bad pending count", __func__);
    }

    q->state = DL_DONE;
    cls.download.pending--;
    Com_DPrintf("%s: %s [%d]\n", __func__, q->path, cls.download.pending);
}

/*
===============
CL_CleanupDownloads

Disconnected from server, clean up.
===============
*/
void CL_CleanupDownloads(void)
{
    dlqueue_t *q, *n;

#if USE_CURL
    HTTP_CleanupDownloads();
#endif

    FOR_EACH_DLQ_SAFE(q, n) {
        Z_Free(q);
    }

    List_Init(&cls.download.queue);
    cls.download.pending = 0;

    cls.download.current = NULL;
    cls.download.percent = 0;

    if (cls.download.file) {
        FS_FCloseFile(cls.download.file);
        cls.download.file = 0;
    }

    cls.download.temp[0] = 0;
}

// start legacy UDP download
static qboolean start_download(dlqueue_t *q)
{
    size_t len;
    qhandle_t f;
    ssize_t ret;

    len = strlen(q->path);
    if (len >= MAX_QPATH) {
        Com_Error(ERR_DROP, "%s: oversize quake path", __func__);
    }

    // download to a temp name, and only rename
    // to the real name when done, so if interrupted
    // a runt file wont be left
    memcpy(cls.download.temp, q->path, len);
    memcpy(cls.download.temp + len, ".tmp", 5);

//ZOID
    // check to see if we already have a tmp for this file, if so, try to resume
    // open the file if not opened yet
    ret = FS_FOpenFile(cls.download.temp, &f, FS_MODE_RDWR);
    if (ret >= 0) {  // it exists
        cls.download.file = f;
        // give the server an offset to start the download
        Com_DPrintf("[UDP] Resuming %s\n", q->path);
        CL_ClientCommand(va("download \"%s\" %d", q->path, (int)ret));
    } else if (ret == Q_ERR_NOENT) {  // it doesn't exist
        Com_DPrintf("[UDP] Downloading %s\n", q->path);
        CL_ClientCommand(va("download \"%s\"", q->path));
    } else { // error happened
        Com_EPrintf("[UDP] Couldn't open %s for appending: %s\n",
                    cls.download.temp, Q_ErrorString(ret));
        CL_FinishDownload(q);
        return qfalse;
    }

    q->state = DL_RUNNING;
    cls.download.current = q;
    return qtrue;
}

/*
===============
CL_StartNextDownload

Start another UDP download if possible
===============
*/
void CL_StartNextDownload(void)
{
    dlqueue_t *q;

    if (!cls.download.pending || cls.download.current) {
        return;
    }

    FOR_EACH_DLQ(q) {
        if (q->state == DL_PENDING) {
            if (start_download(q)) {
                break;
            }
        }
    }
}

/*
=====================
CL_HandleDownload

A download data has been received from the server
=====================
*/
void CL_HandleDownload(const byte *data, int size, int percent)
{
    dlqueue_t *q = cls.download.current;
    const char *msg = NULL;
    qerror_t ret;

    if (!q) {
        Com_Error(ERR_DROP, "%s: no download requested", __func__);
    }

    if (size == -1) {
        if (!percent) {
            msg = "FAIL";
        } else {
            msg = "STOP";
        }
        if (cls.download.file) {
            // if here, we tried to resume a file but the server said no
            FS_FCloseFile(cls.download.file);
        }
        goto another;
    }

    // open the file if not opened yet
    if (!cls.download.file) {
        ret = FS_FOpenFile(cls.download.temp, &cls.download.file, FS_MODE_WRITE);
        if (!cls.download.file) {
            Com_EPrintf("[UDP] Couldn't open %s for writing: %s\n",
                        cls.download.temp, Q_ErrorString(ret));
            goto another;
        }
    }

    ret = FS_Write(data, size, cls.download.file);
    if (ret != size) {
        Com_EPrintf("[UDP] Couldn't write %s: %s\n",
                    cls.download.temp, Q_ErrorString(ret));
        FS_FCloseFile(cls.download.file);
        goto another;
    }

    if (percent != 100) {
        // request next block
        // change display routines by zoid
        cls.download.percent = percent;

        CL_ClientCommand("nextdl");
    } else {
        FS_FCloseFile(cls.download.file);

        // rename the temp file to it's final name
        ret = FS_RenameFile(cls.download.temp, q->path);
        if (ret) {
            Com_EPrintf("[UDP] Couldn't rename %s to %s: %s\n",
                        cls.download.temp, q->path, Q_ErrorString(ret));
        } else {
            msg = "DONE";
        }

another:
        // finished with current path
        CL_FinishDownload(q);
        cls.download.current = NULL;
        cls.download.percent = 0;
        cls.download.file = 0;
        cls.download.temp[0] = 0;

        if (msg) {
            Com_Printf("[UDP] %s [%s] [%d remaining file%s]\n",
                       q->path, msg, cls.download.pending, cls.download.pending == 1 ? "" : "s");
        }

        // get another file if needed
        CL_RequestNextDownload();
        CL_StartNextDownload();
    }
}


/*
===============
CL_CheckDownloadExtension

Only predefined set of filename extensions is allowed,
to prevent the server from uploading arbitrary files.
===============
*/
qboolean CL_CheckDownloadExtension(const char *ext)
{
    static const char allowed[][4] = {
        "pcx", "wal", "wav", "md2", "sp2", "tga", "png",
        "jpg", "bsp", "ent", "txt", "dm2", "loc", "md3"
    };
    static const int total = sizeof(allowed) / sizeof(allowed[0]);
    int i;

    for (i = 0; i < total; i++)
        if (!Q_stricmp(ext, allowed[i]))
            return qtrue;

    return qfalse;
}

// attempts to start a download from the server if file doesn't exist.
static qerror_t check_file_len(const char *path, size_t len, dltype_t type)
{
    char buffer[MAX_QPATH], *ext;
    qerror_t ret;

    // check for oversize path
    if (len >= MAX_QPATH)
        return Q_ERR_NAMETOOLONG;

    // normalize path
    len = FS_NormalizePath(buffer, path);

    // check for empty path
    if (len == 0)
        return Q_ERR_NAMETOOSHORT;

    // check path
    if (!Q_ispath(buffer[0])
        || !Q_ispath(buffer[len - 1])
        || !FS_ValidatePath(buffer)
        || strstr(buffer, "..")
        || !strchr(buffer, '/')) {
        // some of these checks are too conservative or even redundant
        // once we have normalized the path, however they have to stay for
        // compatibility reasons with older servers (some would even ban us
        // for sending .. for example)
        return Q_ERR_INVALID_PATH;
    }

    // check extension
    ext = COM_FileExtension(buffer);
    if (*ext != '.' || !CL_CheckDownloadExtension(ext + 1))
        return Q_ERR_INVALID_PATH;

    if (FS_FileExists(buffer))
        // it exists, no need to download
        return Q_ERR_EXIST;

#if USE_CURL
    ret = HTTP_QueueDownload(buffer, type);
    if (ret != Q_ERR_NOSYS)
        return ret;
#endif

    // queue and start legacy UDP download
    ret = CL_QueueDownload(buffer, type);
    if (ret == Q_ERR_SUCCESS)
        CL_StartNextDownload();

    return ret;
}

#define check_file(path, type) \
    check_file_len(path, strlen(path), type)

static void check_skins(const char *name)
{
    size_t          i, num_skins, ofs_skins, end_skins;
    byte            *model;
    size_t          len;
    dmd2header_t    *md2header;
    dsp2header_t    *sp2header;
    char            *md2skin;
    dsp2frame_t     *sp2frame;
    uint32_t        ident;
    char            fn[MAX_QPATH];

    len = FS_LoadFile(name, (void **)&model);
    if (!model) {
        // couldn't load it
        return;
    }

    if (len < sizeof(ident)) {
        // file too small
        goto done;
    }

    // check ident
    ident = LittleLong(*(uint32_t *)model);
    switch (ident) {
    case MD2_IDENT:
        // alias model
        md2header = (dmd2header_t *)model;
        if (len < sizeof(*md2header) ||
            LittleLong(md2header->ident) != MD2_IDENT ||
            LittleLong(md2header->version) != MD2_VERSION) {
            // not an alias model
            goto done;
        }

        num_skins = LittleLong(md2header->num_skins);
        ofs_skins = LittleLong(md2header->ofs_skins);
        end_skins = ofs_skins + num_skins * MD2_MAX_SKINNAME;
        if (num_skins > MD2_MAX_SKINS || end_skins < ofs_skins || end_skins > len) {
            // bad alias model
            goto done;
        }

        md2skin = (char *)model + ofs_skins;
        for (i = 0; i < num_skins; i++) {
            if (!Q_memccpy(fn, md2skin, 0, sizeof(fn))) {
                // bad alias model
                goto done;
            }
            check_file(fn, DL_OTHER);
            md2skin += MD2_MAX_SKINNAME;
        }
        break;

    case SP2_IDENT:
        // sprite model
        sp2header = (dsp2header_t *)model;
        if (len < sizeof(*sp2header) ||
            LittleLong(sp2header->ident) != SP2_IDENT ||
            LittleLong(sp2header->version) != SP2_VERSION) {
            // not a sprite model
            goto done;
        }

        num_skins = LittleLong(sp2header->numframes);
        ofs_skins = sizeof(*sp2header);
        end_skins = ofs_skins + num_skins * sizeof(dsp2frame_t);
        if (num_skins > SP2_MAX_FRAMES || end_skins < ofs_skins || end_skins > len) {
            // bad sprite model
            goto done;
        }

        sp2frame = (dsp2frame_t *)(model + ofs_skins);
        for (i = 0; i < num_skins; i++) {
            if (!Q_memccpy(fn, sp2frame->name, 0, sizeof(fn))) {
                // bad sprite model
                goto done;
            }
            check_file(fn, DL_OTHER);
            sp2frame++;
        }
        break;

    default:
        // unknown file format
        goto done;
    }

done:
    FS_FreeFile(model);
}

static void check_player(const char *name)
{
    char fn[MAX_QPATH], model[MAX_QPATH], skin[MAX_QPATH], *p;
    size_t len;
    int i, j;

    CL_ParsePlayerSkin(NULL, model, skin, name);

    // model
    len = Q_concat(fn, sizeof(fn), "players/", model, "/tris.md2", NULL);
    check_file_len(fn, len, DL_OTHER);

    // weapon models
    for (i = 0; i < cl.numWeaponModels; i++) {
        len = Q_concat(fn, sizeof(fn), "players/", model, "/", cl.weaponModels[i], NULL);
        check_file_len(fn, len, DL_OTHER);
    }

    // default weapon skin
    len = Q_concat(fn, sizeof(fn), "players/", model, "/weapon.pcx", NULL);
    check_file_len(fn, len, DL_OTHER);

    // skin
    len = Q_concat(fn, sizeof(fn), "players/", model, "/", skin, ".pcx", NULL);
    check_file_len(fn, len, DL_OTHER);

    // skin_i
    len = Q_concat(fn, sizeof(fn), "players/", model, "/", skin, "_i.pcx", NULL);
    check_file_len(fn, len, DL_OTHER);

    // sexed sounds
    for (i = 0; i < precache_sexed_total; i++) {
        j = precache_sexed_sounds[i];
        p = cl.configstrings[CS_SOUNDS + j];

        if (*p == '*') {
            len = Q_concat(fn, sizeof(fn), "players/", model, "/", p + 1, NULL);
            check_file_len(fn, len, DL_OTHER);
        }
    }
}

// for precaching dependencies
static qboolean downloads_pending(dltype_t type)
{
    dlqueue_t *q;

    // DL_OTHER just checks for any download
    if (type == DL_OTHER) {
        return !!cls.download.pending;
    }

    // see if there are pending downloads of the given type
    FOR_EACH_DLQ(q) {
        if (q->state != DL_DONE && q->type == type) {
            return qtrue;
        }
    }

    return qfalse;
}

/*
=====================
CL_RequestNextDownload

Runs precache check and dispatches downloads.
=====================
*/
void CL_RequestNextDownload(void)
{
    char fn[MAX_QPATH], *name;
    size_t len;
    int i;

    if (cls.state != ca_connected && cls.state != ca_loading)
        return;

    if (allow_download->integer <= 0 || NET_IsLocalAddress(&cls.serverAddress)) {
        if (precache_check <= PRECACHE_MAP) {
            CL_RegisterBspModels();
        }

        CL_Begin();
        return;
    }

    switch (precache_check) {
    case PRECACHE_MODELS:
        // confirm map
        if (allow_download_maps->integer)
            check_file(cl.configstrings[CS_MODELS + 1], DL_MAP);

        // checking for models
        if (allow_download_models->integer) {
            for (i = 2; i < MAX_MODELS; i++) {
                name = cl.configstrings[CS_MODELS + i];
                if (!name[0]) {
                    break;
                }
                if (name[0] == '*' || name[0] == '#') {
                    continue;
                }
                check_file(name, DL_MODEL);
            }
        }

        precache_check = PRECACHE_OTHER;
        // fall through

    case PRECACHE_OTHER:
        if (allow_download_models->integer) {
            if (downloads_pending(DL_MODEL)) {
                // pending downloads (models), let's wait here before we can check skins.
                Com_DPrintf("%s: waiting for models...\n", __func__);
                return;
            }

            for (i = 2; i < MAX_MODELS; i++) {
                name = cl.configstrings[CS_MODELS + i];
                if (!name[0]) {
                    break;
                }
                if (name[0] == '*' || name[0] == '#') {
                    continue;
                }
                check_skins(name);
            }
        }

        if (allow_download_sounds->integer) {
            for (i = 1; i < MAX_SOUNDS; i++) {
                name = cl.configstrings[CS_SOUNDS + i];
                if (!name[0]) {
                    break;
                }
                if (name[0] == '*') {
                    continue;
                }
                if (name[0] == '#') {
                    len = Q_strlcpy(fn, name + 1, sizeof(fn));
                } else {
                    len = Q_concat(fn, sizeof(fn), "sound/", name, NULL);
                }
                check_file_len(fn, len, DL_OTHER);
            }
        }

        if (allow_download_pics->integer) {
            for (i = 1; i < MAX_IMAGES; i++) {
                name = cl.configstrings[CS_IMAGES + i];
                if (!name[0]) {
                    break;
                }
                if (name[0] == '/' || name[0] == '\\') {
                    len = Q_strlcpy(fn, name + 1, sizeof(fn));
                } else {
                    len = Q_concat(fn, sizeof(fn), "pics/", name, ".pcx", NULL);
                }
                check_file_len(fn, len, DL_OTHER);
            }
        }

        if (allow_download_players->integer) {
            // find sexed sounds
            precache_sexed_total = 0;
            for (i = 1; i < MAX_SOUNDS; i++) {
                if (cl.configstrings[CS_SOUNDS + i][0] == '*') {
                    precache_sexed_sounds[precache_sexed_total++] = i;
                }
            }

            for (i = 0; i < MAX_CLIENTS; i++) {
                name = cl.configstrings[CS_PLAYERSKINS + i];
                if (!name[0]) {
                    continue;
                }
                check_player(name);
            }
        }

        if (allow_download_textures->integer) {
            static const char env_suf[6][3] = {
                "rt", "bk", "lf", "ft", "up", "dn"
            };

            for (i = 0; i < 12; i++) {
                if (i & 1)
                    len = Q_concat(fn, sizeof(fn),
                                   "env/", cl.configstrings[CS_SKY], env_suf[i >> 1], ".pcx", NULL);
                else
                    len = Q_concat(fn, sizeof(fn),
                                   "env/", cl.configstrings[CS_SKY], env_suf[i >> 1], ".tga", NULL);
                check_file_len(fn, len, DL_OTHER);
            }
        }

        precache_check = PRECACHE_MAP;
        // fall through

    case PRECACHE_MAP:
        if (downloads_pending(DL_MAP)) {
            // map might still be downloading?
            Com_DPrintf("%s: waiting for map...\n", __func__);
            return;
        }

        // load the map file before checking textures
        CL_RegisterBspModels();

        if (allow_download_textures->integer) {
            for (i = 0; i < cl.bsp->numtexinfo; i++) {
                len = Q_concat(fn, sizeof(fn), "textures/", cl.bsp->texinfo[i].name, ".wal", NULL);
                check_file_len(fn, len, DL_OTHER);
            }
        }

        precache_check = PRECACHE_FINAL;
        // fall through

    case PRECACHE_FINAL:
        if (downloads_pending(DL_OTHER)) {
            // pending downloads (possibly textures), let's wait here.
            Com_DPrintf("%s: waiting for others...\n", __func__);
            return;
        }

        // all done, tell server we are ready
        CL_Begin();
        break;

    default:
        Com_Error(ERR_DROP, "%s: bad precache_check\n", __func__);
    }
}

void CL_ResetPrecacheCheck(void)
{
    precache_check = PRECACHE_MODELS;
}

/*
===============
CL_Download_f

Request a download from the server
===============
*/
static void CL_Download_f(void)
{
    char *path;
    qerror_t ret;

    if (cls.state < ca_connected) {
        Com_Printf("Must be connected to a server.\n");
        return;
    }

    if (allow_download->integer == -1) {
        Com_Printf("Downloads are permanently disabled.\n");
        return;
    }

    if (Cmd_Argc() != 2) {
        Com_Printf("Usage: download <filename>\n");
        return;
    }

    path = Cmd_Argv(1);

    ret = check_file(path, DL_OTHER);
    if (ret) {
        Com_Printf("Couldn't download %s: %s\n", path, Q_ErrorString(ret));
    }
}

void CL_InitDownloads(void)
{
    Cmd_AddCommand("download", CL_Download_f);

    List_Init(&cls.download.queue);
}

