/* Copyright (C) 2009 Trend Micro Inc.
 * All right reserved.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

/* Rootcheck decoder */

#include "config.h"
#include "os_regex/os_regex.h"
#include "eventinfo.h"
#include "alerts/alerts.h"
#include "decoder.h"
#include "wazuh_db/wdb.h"

#define ROOTCHECK_DIR    "/queue/rootcheck"

/* Rootcheck fields */
#define RCK_TITLE   0
#define RCK_FILE    1
#define RCK_NFIELDS 2

/* Local variables */
static char *rk_agent_ips[MAX_AGENTS];
static FILE *rk_agent_fps[MAX_AGENTS];
static int rk_err;

/* Rootcheck decoder */
static OSDecoderInfo *rootcheck_dec = NULL;

/* Get rootcheck title from log */
static char* rk_get_title(const char *log);

/* Get rootcheck file from log */
static char* rk_get_file(const char *log);

/* Initialize the necessary information to process the rootcheck information */
void RootcheckInit()
{
    int i = 0;

    rk_err = 0;

    for (; i < MAX_AGENTS; i++) {
        rk_agent_ips[i] = NULL;
        rk_agent_fps[i] = NULL;
    }

    /* Zero decoder */
    os_calloc(1, sizeof(OSDecoderInfo), rootcheck_dec);
    rootcheck_dec->id = getDecoderfromlist(ROOTCHECK_MOD);
    rootcheck_dec->type = OSSEC_RL;
    rootcheck_dec->name = ROOTCHECK_MOD;
    rootcheck_dec->fts = 0;

    /* New fields as dynamic */

    os_calloc(Config.decoder_order_size, sizeof(char *), rootcheck_dec->fields);
    rootcheck_dec->fields[RCK_TITLE] = "title";
    rootcheck_dec->fields[RCK_FILE] = "file";

    debug1("%s: RootcheckInit completed.", ARGV0);

    return;
}

/* Return the file pointer to be used */
static FILE *RK_File(const char *agent, int *agent_id)
{
    int i = 0;
    char rk_buf[OS_SIZE_1024 + 1];

    while (rk_agent_ips[i] != NULL) {
        if (strcmp(rk_agent_ips[i], agent) == 0) {
            /* Pointing to the beginning of the file */
            fseek(rk_agent_fps[i], 0, SEEK_SET);
            *agent_id = i;
            return (rk_agent_fps[i]);
        }

        i++;
    }

    /* If here, our agent wasn't found */
    rk_agent_ips[i] = strdup(agent);

    if (rk_agent_ips[i] != NULL) {
        snprintf(rk_buf, OS_SIZE_1024, "%s/%s", ROOTCHECK_DIR, agent);

        /* r+ to read and write. Do not truncate */
        rk_agent_fps[i] = fopen(rk_buf, "r+");
        if (!rk_agent_fps[i]) {
            /* Try opening with a w flag, file probably does not exist */
            rk_agent_fps[i] = fopen(rk_buf, "w");
            if (rk_agent_fps[i]) {
                fclose(rk_agent_fps[i]);
                rk_agent_fps[i] = fopen(rk_buf, "r+");
            }
        }
        if (!rk_agent_fps[i]) {
            merror(FOPEN_ERROR, ARGV0, rk_buf, errno, strerror(errno));

            free(rk_agent_ips[i]);
            rk_agent_ips[i] = NULL;

            return (NULL);
        }

        /* Return the opened pointer (the beginning of it) */
        fseek(rk_agent_fps[i], 0, SEEK_SET);
        *agent_id = i;
        return (rk_agent_fps[i]);
    }

    else {
        merror(MEM_ERROR, ARGV0, errno, strerror(errno));
        return (NULL);
    }

    return (NULL);
}

/* Special decoder for rootcheck
 * Not using the default rendering tools for simplicity
 * and to be less resource intensive
 */
int DecodeRootcheck(Eventinfo *lf)
{
    int agent_id;

    char *tmpstr;
    char rk_buf[OS_SIZE_2048 + 1];

    FILE *fp;

    fpos_t fp_pos;

    /* Zero rk_buf */
    rk_buf[0] = '\0';
    rk_buf[OS_SIZE_2048] = '\0';

    fp = RK_File(lf->location, &agent_id);

    if (!fp) {
        merror("%s: Error handling rootcheck database.", ARGV0);
        rk_err++;

        return (0);
    }

    /* Get initial position */
    if (fgetpos(fp, &fp_pos) == -1) {
        merror("%s: Error handling rootcheck database (fgetpos).", ARGV0);
        return (0);
    }


    /* Reads the file and search for a possible entry */
    while (fgets(rk_buf, OS_SIZE_2048 - 1, fp) != NULL) {
        /* Ignore blank lines and lines with a comment */
        if (rk_buf[0] == '\n' || rk_buf[0] == '#') {
            if (fgetpos(fp, &fp_pos) == -1) {
                merror("%s: Error handling rootcheck database "
                       "(fgetpos2).", ARGV0);
                return (0);
            }
            continue;
        }

        /* Remove newline */
        tmpstr = strchr(rk_buf, '\n');
        if (tmpstr) {
            *tmpstr = '\0';
        }

        /* Old format without the time stamps */
        if (rk_buf[0] != '!') {
            /* Cannot use strncmp to avoid errors with crafted files */
            if (strcmp(lf->log, rk_buf) == 0) {
                rootcheck_dec->fts = 0;
                lf->decoder_info = rootcheck_dec;
                lf->nfields = RCK_NFIELDS;
                lf->fields[RCK_TITLE].key = rootcheck_dec->fields[RCK_TITLE];
                lf->fields[RCK_TITLE].value = rk_get_title(lf->log);
                lf->fields[RCK_FILE].key = rootcheck_dec->fields[RCK_FILE];
                lf->fields[RCK_FILE].value = rk_get_file(lf->log);
                wdb_update_pm(lf->agent_id ? atoi(lf->agent_id) : 0, lf->location, lf->log, (long int)lf->time);
                return (1);
            }
        }
        /* New format */
        else {
            /* Going past time: !1183431603!1183431603  (last, first seen) */
            tmpstr = rk_buf + 23;

            /* Matches, we need to upgrade last time saw */
            if (strcmp(lf->log, tmpstr) == 0) {
                if(fsetpos(fp, &fp_pos)) {
                    merror("%s: Error handling rootcheck database "
                           "(fsetpos).", ARGV0);
                    return (0);
                }
                fprintf(fp, "!%ld", (long int)lf->time);
                rootcheck_dec->fts = 0;
                lf->decoder_info = rootcheck_dec;
                lf->nfields = RCK_NFIELDS;
                lf->fields[RCK_TITLE].key = rootcheck_dec->fields[RCK_TITLE];
                lf->fields[RCK_TITLE].value = rk_get_title(lf->log);
                lf->fields[RCK_FILE].key = rootcheck_dec->fields[RCK_FILE];
                lf->fields[RCK_FILE].value = rk_get_file(lf->log);
                wdb_update_pm(lf->agent_id ? atoi(lf->agent_id) : 0, lf->location, lf->log, (long int)lf->time);
                return (1);
            }
        }

        /* Get current position */
        if (fgetpos(fp, &fp_pos) == -1) {
            merror("%s: Error handling rootcheck database (fgetpos3).", ARGV0);
            return (0);
        }
    }

    /* Add the new entry at the end of the file */
    fseek(fp, 0, SEEK_END);
    fprintf(fp, "!%ld!%ld %s\n", (long int)lf->time, (long int)lf->time, lf->log);
    fflush(fp);

    rootcheck_dec->fts = FTS_DONE;
    lf->decoder_info = rootcheck_dec;
    lf->nfields = RCK_NFIELDS;
    lf->fields[RCK_TITLE].key = rootcheck_dec->fields[RCK_TITLE];
    lf->fields[RCK_TITLE].value = rk_get_title(lf->log);
    lf->fields[RCK_FILE].key = rootcheck_dec->fields[RCK_FILE];
    lf->fields[RCK_FILE].value = rk_get_file(lf->log);
    wdb_insert_pm(lf->agent_id ? atoi(lf->agent_id) : 0, lf->location, (long int)lf->time, lf->log);
    return (1);
}

/* Get rootcheck title from log */

char* rk_get_title(const char *log) {
    char *title = strdup(log);
    char *c;
    char *d;
    char *orig;

    if ((c = strstr(title, " {"))) {
        if (c == title) {
            free(title);
            return NULL;
        } else
            *c = '\0';
    }

    if ((c = strstr(title, "System Audit: ")) && (!(d = strstr(title, " - ")) || c < d )) {
        orig = title;
        title = strdup(c + 14);
        free(orig);
    }

    // Remove "\. .*"

    if ((c = strstr(title, ". "))) {
        c[1] = '\0';
    }

    // Remove "File: ('.*') "

    if (((c = strstr(title, "File '")) || (c = strstr(title, "file '"))) && (d = strstr(c + 6, "' "))) {
        memmove(c + 5, d + 2, strlen(d + 2) + 1);
    }

    return title;
}

/* Get rootcheck file from log */

char* rk_get_file(const char *log) {
    char *c;
    char *file;

    if ((file = strstr(log, "File: "))) {
        file += 6;

        if ((c = strstr(file, ". "))) {
            *c = '\0';
            return strdup(file);
        } else
            return NULL;
    } else if ((file = strstr(log, "File '")) || (file = strstr(log, "file '"))) {
        file += 6;

        if ((c = strstr(file, "' "))) {
            *c = '\0';
            return strdup(file);
        } else
            return NULL;
    } else
        return NULL;
}
