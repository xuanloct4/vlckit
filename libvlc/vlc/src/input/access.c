/*****************************************************************************
 * access.c
 *****************************************************************************
 * Copyright (C) 1999-2008 VLC authors and VideoLAN
 * $Id$
 *
 * Author: Laurent Aimar <fenrir _AT_ videolan _DOT_ org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_url.h>
#include <vlc_modules.h>
#include <vlc_interrupt.h>

#include <libvlc.h>
#include "stream.h"
#include "input_internal.h"

struct vlc_access_private
{
    module_t *module;
};

/* Decode URL (which has had its scheme stripped earlier) to a file path. */
char *get_path(const char *location)
{
    char *url, *path;

    /* Prepending "file://" is a bit hackish. But then again, we do not want
     * to hard-code the list of schemes that use file paths in vlc_uri2path().
     */
    if (asprintf(&url, "file://%s", location) == -1)
        return NULL;

    path = vlc_uri2path (url);
    free (url);
    return path;
}

static void vlc_access_Destroy(stream_t *access)
{
    struct vlc_access_private *priv = vlc_stream_Private(access);

    module_unneed(access, priv->module);
    free(access->psz_filepath);
    free(access->psz_name);
}

#define MAX_REDIR 5

/*****************************************************************************
 * access_New:
 *****************************************************************************/
static stream_t *access_New(vlc_object_t *parent, input_thread_t *input,
                            es_out_t *out, bool preparsing, const char *mrl)
{
    struct vlc_access_private *priv;
    char *redirv[MAX_REDIR];
    unsigned redirc = 0;

    stream_t *access = vlc_stream_CustomNew(parent, vlc_access_Destroy,
                                            sizeof (*priv), "access");
    if (unlikely(access == NULL))
        return NULL;

    access->p_input = input;
    access->out = out;
    access->psz_name = NULL;
    access->psz_url = strdup(mrl);
    access->psz_filepath = NULL;
    access->b_preparsing = preparsing;
    priv = vlc_stream_Private(access);

    if (unlikely(access->psz_url == NULL))
        goto error;

    while (redirc < MAX_REDIR)
    {
        char *url = access->psz_url;
        msg_Dbg(access, "creating access: %s", url);

        const char *p = strstr(url, "://");
        if (p == NULL)
            goto error;

        access->psz_name = strndup(url, p - url);
        if (unlikely(access->psz_name == NULL))
            goto error;

        access->psz_location = p + 3;
        access->psz_filepath = get_path(access->psz_location);
        if (access->psz_filepath != NULL)
            msg_Dbg(access, " (path: %s)", access->psz_filepath);

        priv->module = module_need(access, "access", access->psz_name, true);
        if (priv->module != NULL) /* success */
        {
            while (redirc > 0)
                free(redirv[--redirc]);

            assert(access->pf_control != NULL);
            return access;
        }

        if (access->psz_url == url) /* failure (no redirection) */
            goto error;

        /* redirection */
        msg_Dbg(access, "redirecting to: %s", access->psz_url);
        redirv[redirc++] = url;

        for (unsigned j = 0; j < redirc; j++)
            if (!strcmp(redirv[j], access->psz_url))
            {
                msg_Err(access, "redirection loop");
                goto error;
            }

        free(access->psz_filepath);
        free(access->psz_name);
        access->psz_filepath = access->psz_name = NULL;
    }

    msg_Err(access, "too many redirections");
error:
    while (redirc > 0)
        free(redirv[--redirc]);
    free(access->psz_filepath);
    free(access->psz_name);
    stream_CommonDelete(access);
    return NULL;
}

stream_t *vlc_access_NewMRL(vlc_object_t *parent, const char *mrl)
{
    return access_New(parent, NULL, NULL, false, mrl);
}

/*****************************************************************************
 * access_vaDirectoryControlHelper:
 *****************************************************************************/
int access_vaDirectoryControlHelper( stream_t *p_access, int i_query, va_list args )
{
    VLC_UNUSED( p_access );

    switch( i_query )
    {
        case STREAM_CAN_SEEK:
        case STREAM_CAN_FASTSEEK:
        case STREAM_CAN_PAUSE:
        case STREAM_CAN_CONTROL_PACE:
            *va_arg( args, bool* ) = false;
            break;
        case STREAM_GET_PTS_DELAY:
            *va_arg( args, int64_t * ) = 0;
            break;
        default:
            return VLC_EGENERIC;
     }
     return VLC_SUCCESS;
}

/* Block access */
static block_t *AStreamReadBlock(stream_t *s, bool *restrict eof)
{
    stream_t *access = s->p_sys;
    input_thread_t *input = s->p_input;
    block_t * block;

    if (vlc_stream_Eof(access))
    {
        *eof = true;
        return NULL;
    }
    if (vlc_killed())
        return NULL;

    block = vlc_stream_ReadBlock(access);

    if (block != NULL && input != NULL)
    {
        struct input_stats *stats = input_priv(input)->stats;
        if (stats != NULL)
            input_rate_Add(&stats->input_bitrate, block->i_buffer);
    }

    return block;
}

/* Read access */
static ssize_t AStreamReadStream(stream_t *s, void *buf, size_t len)
{
    stream_t *access = s->p_sys;
    input_thread_t *input = s->p_input;

    if (vlc_stream_Eof(access))
        return 0;
    if (vlc_killed())
        return -1;

    ssize_t val = vlc_stream_ReadPartial(access, buf, len);

    if (val > 0 && input != NULL)
    {
        struct input_stats *stats = input_priv(input)->stats;
        if (stats != NULL)
            input_rate_Add(&stats->input_bitrate, val);
    }

    return val;
}

/* Common */
static int AStreamSeek(stream_t *s, uint64_t offset)
{
    stream_t *access = s->p_sys;

    return vlc_stream_Seek(access, offset);
}

static int AStreamControl(stream_t *s, int cmd, va_list args)
{
    stream_t *access = s->p_sys;

    return vlc_stream_vaControl(access, cmd, args);
}

static void AStreamDestroy(stream_t *s)
{
    stream_t *access = s->p_sys;

    vlc_stream_Delete(access);
}

stream_t *stream_AccessNew(vlc_object_t *parent, input_thread_t *input,
                           es_out_t *out, bool preparsing, const char *url)
{
    stream_t *access = access_New(parent, input, out, preparsing, url);
    if (access == NULL)
        return NULL;

    stream_t *s;

    if (access->pf_block != NULL || access->pf_read != NULL)
    {
        s = vlc_stream_CommonNew(VLC_OBJECT(access), AStreamDestroy);
        if (unlikely(s == NULL))
        {
            vlc_stream_Delete(access);
            return NULL;
        }

        s->p_input = input;
        s->psz_url = strdup(access->psz_url);

        if (access->pf_block != NULL)
            s->pf_block = AStreamReadBlock;
        if (access->pf_read != NULL)
            s->pf_read = AStreamReadStream;

        s->pf_seek = AStreamSeek;
        s->pf_control = AStreamControl;
        s->p_sys = access;

        s = stream_FilterChainNew(s, "prefetch,cache");
    }
    else
        s = access;

    return s;
}
