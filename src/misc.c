/* createrepo_c - Library of routines for manipulation with repodata
 * Copyright (C) 2012  Tomas Mlcoch
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#define _XOPEN_SOURCE 500

#include <glib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ftw.h>
#include <curl/curl.h>
#include "logging.h"
#include "constants.h"
#include "misc.h"

#undef MODULE
#define MODULE "misc: "


#define BUFFER_SIZE     4096


const char *flag_to_string(gint64 flags)
{
    flags &= 0xf;
    switch(flags) {
        case 0:
            return "";
        case 2:
            return "LT";
        case 4:
            return "GT";
        case 8:
            return "EQ";
        case 10:
            return "LE";
        case 12:
            return "GE";
        default:
            return "";
    }
}



/*
 * BE CAREFUL!
 *
 * In case chunk param is NULL:
 * Returned structure had all strings malloced!!!
 * Be so kind and don't forget use free() for all its element, before end of structure lifecycle.
 *
 * In case chunk is pointer to a GStringChunk:
 * Returned structure had all string inserted in the passed chunk.
 *
 */
struct EVR string_to_version(const char *string, GStringChunk *chunk)
{
    struct EVR evr;
    evr.epoch = NULL;
    evr.version = NULL;
    evr.release = NULL;

    if (!string || !(strlen(string))) {
        return evr;
    }

    const char *ptr;  // These names are totally self explaining
    const char *ptr2; //


    // Epoch

    ptr = strstr(string, ":");
    if (ptr) {
        // Check if epoch str is a number
        char *p = NULL;
        strtol(string, &p, 10);
        if (p == ptr) { // epoch str seems to be a number
            size_t len = ptr - string;
            if (len) {
                if (chunk) {
                    evr.epoch = g_string_chunk_insert_len(chunk, string, len);
                } else {
                    evr.epoch = g_strndup(string, len);
                }
            }
        }
    } else { // There is no epoch
        ptr = (char*) string-1;
    }

    if (!evr.epoch) {
        if (chunk) {
            evr.epoch = g_string_chunk_insert_const(chunk, "0");
        } else {
            evr.epoch = g_strdup("0");
        }
    }


    // Version + release

    ptr2 = strstr(ptr+1, "-");
    if (ptr2) {
        // Version
        size_t version_len = ptr2 - (ptr+1);
        if (chunk) {
            evr.version = g_string_chunk_insert_len(chunk, ptr+1, version_len);
        } else {
            evr.version = g_strndup(ptr+1, version_len);
        }

        // Release
        size_t release_len = strlen(ptr2+1);
        if (release_len) {
            if (chunk) {
                evr.release = g_string_chunk_insert_len(chunk, ptr2+1, release_len);
            } else {
                evr.release = g_strndup(ptr2+1, release_len);
            }
        }
    } else { // Release is not here, just version
        if (chunk) {
            evr.version = g_string_chunk_insert_const(chunk, ptr+1);
        } else {
            evr.version = g_strdup(ptr+1);
        }
    }

    return evr;
}



inline int is_primary(const char *filename)
{
/*
    This optimal piece of code cannot be used because of yum...
    We must match any string that contains "bin/" in dirname

    Response to my question from packaging team:
    ....
    It must still contain that. Atm. it's defined as taking anything
    with 'bin/' in the path. The idea was that it'd match /usr/kerberos/bin/
    and /opt/blah/sbin. So that is what all versions of createrepo generate,
    and what yum all versions of yum expect to be generated.
    We can't change one side, without breaking the expectation of the
    other.
    There have been plans to change the repodata, and one of the changes
    would almost certainly be in how files are represented ... likely via.
    lists of "known" paths, that can be computed at createrepo time.

    if (!strncmp(filename, "/bin/", 5)) {
        return 1;
    }

    if (!strncmp(filename, "/sbin/", 6)) {
        return 1;
    }

    if (!strncmp(filename, "/etc/", 5)) {
        return 1;
    }

    if (!strncmp(filename, "/usr/", 5)) {
        if (!strncmp(filename+5, "bin/", 4)) {
            return 1;
        }

        if (!strncmp(filename+5, "sbin/", 5)) {
            return 1;
        }

        if (!strcmp(filename+5, "lib/sendmail")) {
            return 1;
        }
    }
*/

    if (!strncmp(filename, "/etc/", 5)) {
        return 1;
    }

    if (!strcmp(filename, "/usr/lib/sendmail")) {
        return 1;
    }

    if (strstr(filename, "bin/")) {
        return 1;
    }

    return 0;
}



char *compute_file_checksum(const char *filename, ChecksumType type)
{
    GChecksumType gchecksumtype;

    if (!filename) {
        g_debug(MODULE"%s: Filename param is NULL", __func__);
        return NULL;
    }

    // Check if file exists and if it is a regular file (not a directory)

    if (!g_file_test(filename, (G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))) {
        g_debug(MODULE"%s: File %s doesn't exists", __func__, filename);
        return NULL;
    }


    // Convert our checksum type into glib type

    switch (type) {
        case PKG_CHECKSUM_MD5:
            gchecksumtype = G_CHECKSUM_MD5;
            break;
        case PKG_CHECKSUM_SHA1:
            gchecksumtype = G_CHECKSUM_SHA1;
            break;
        case PKG_CHECKSUM_SHA256:
            gchecksumtype = G_CHECKSUM_SHA256;
            break;
        default:
            g_debug(MODULE"%s: Unknown checksum type", __func__);
            return NULL;
    };


    // Open file and initialize checksum structure

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        g_critical(MODULE"%s: Cannot open %s (%s)", __func__, filename, strerror(errno));
        return NULL;
    }


    // Calculate checksum

    GChecksum *checksum = g_checksum_new(gchecksumtype);
    unsigned char buffer[BUFFER_SIZE];

    while (1) {
        size_t input_len;
        input_len = fread((void *) buffer, sizeof(unsigned char), BUFFER_SIZE, fp);
        g_checksum_update(checksum, (const guchar *) buffer, input_len);
        if (input_len < BUFFER_SIZE) {
            break;
        }
    }

    fclose(fp);


    // Get checksum

    char *checksum_str = g_strdup(g_checksum_get_string(checksum));
    g_checksum_free(checksum);

    if (!checksum_str) {
        g_critical(MODULE"%s: Cannot get checksum %s (low memory?)", __func__, filename);
    }

    return checksum_str;
}



#define VAL_LEN         4       // Len of numeric values in rpm

struct HeaderRangeStruct get_header_byte_range(const char *filename)
{
    /* Values readed by fread are 4 bytes long and stored as big-endian.
     * So there is htonl function to convert this big-endian number into host byte order.
     */

    struct HeaderRangeStruct results;

    results.start = 0;
    results.end   = 0;


    // Open file

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        g_debug(MODULE"%s: Cannot open file %s (%s)", __func__, filename, strerror(errno));
        return results;
    }


    // Get header range

    if (fseek(fp, 104, SEEK_SET) != 0) {
        g_debug(MODULE"%s: fseek fail on %s (%s)", __func__, filename, strerror(errno));
        fclose(fp);
        return results;
    }

    unsigned int sigindex = 0;
    unsigned int sigdata  = 0;
    fread(&sigindex, VAL_LEN, 1, fp);
    sigindex = htonl(sigindex);
    fread(&sigdata, VAL_LEN, 1, fp);
    sigdata = htonl(sigdata);
    unsigned int sigindexsize = sigindex * 16;
    unsigned int sigsize = sigdata + sigindexsize;
    unsigned int disttoboundary = sigsize % 8;
    if (disttoboundary) {
        disttoboundary = 8 - disttoboundary;
    }
    unsigned int hdrstart = 112 + sigsize + disttoboundary;

    fseek(fp, hdrstart, SEEK_SET);
    fseek(fp, 8, SEEK_CUR);

    unsigned int hdrindex = 0;
    unsigned int hdrdata  = 0;
    fread(&hdrindex, VAL_LEN, 1, fp);
    hdrindex = htonl(hdrindex);
    fread(&hdrdata, VAL_LEN, 1, fp);
    hdrdata = htonl(hdrdata);
    unsigned int hdrindexsize = hdrindex * 16;
    unsigned int hdrsize = hdrdata + hdrindexsize + 16;
    unsigned int hdrend = hdrstart + hdrsize;

    fclose(fp);


    // Check sanity

    if (hdrend < hdrstart) {
        g_debug(MODULE"%s: sanity check fail on %s (%d > %d))", __func__, filename, hdrstart, hdrend);
        return results;
    }

    results.start = hdrstart;
    results.end   = hdrend;

    return results;
}


const char *get_checksum_name_str(ChecksumType type)
{
    char *name = NULL;

    switch (type) {
        case PKG_CHECKSUM_MD5:
            name = "md5";
            break;
        case PKG_CHECKSUM_SHA1:
            name = "sha1";
            break;
        case PKG_CHECKSUM_SHA256:
            name = "sha256";
            break;
        default:
            g_debug(MODULE"%s: Unknown checksum (%d)", __func__, type);
            break;
    }

    return name;
}


char *get_filename(const char *filepath)
{
    char *filename = NULL;

    if (!filepath)
        return filename;

    filename = (char *) filepath;
    size_t x = 0;

    while (filepath[x] != '\0') {
        if (filepath[x] == '/') {
            filename = (char *) filepath+(x+1);
        }
        x++;
    }

    return filename;
}


int copy_file(const char *src, const char *in_dst)
{
    size_t readed;
    char buf[BUFFER_SIZE];

    FILE *orig;
    FILE *new;

    if (!src || !in_dst) {
        g_debug(MODULE"%s: File name cannot be NULL", __func__);
        return CR_COPY_ERR;
    }

    // If destination is dir use filename from src
    gchar *dst = (gchar *) in_dst;
    if (g_str_has_suffix(in_dst, "/")) {
        dst = g_strconcat(in_dst, get_filename(src), NULL);
    }

    if ((orig = fopen(src, "r")) == NULL) {
        g_debug(MODULE"%s: Cannot open source file %s (%s)", __func__, src, strerror(errno));
        return CR_COPY_ERR;
    }

    if ((new = fopen(dst, "w")) == NULL) {
        g_debug(MODULE"%s: Cannot open destination file %s (%s)", __func__, dst, strerror(errno));
        fclose(orig);
        return CR_COPY_ERR;
    }

    while ((readed = fread(buf, 1, BUFFER_SIZE, orig)) > 0) {
        if (fwrite(buf, 1, readed, new) != readed) {
            g_debug(MODULE"%s: Error while copy %s -> %s (%s)", __func__, src, dst, strerror(errno));
            fclose(new);
            fclose(orig);
            return CR_COPY_ERR;
        }

        if (readed != BUFFER_SIZE && ferror(orig)) {
            g_debug(MODULE"%s: Error while copy %s -> %s (%s)", __func__, src, dst, strerror(errno));
            fclose(new);
            fclose(orig);
            return CR_COPY_ERR;
        }
    }

    if (dst != in_dst) {
        g_free(dst);
    }

    fclose(new);
    fclose(orig);

    return CR_COPY_OK;
}



int compress_file(const char *src, const char *in_dst, CompressionType compression)
{
    int readed;
    char buf[BUFFER_SIZE];

    FILE *orig;
    CW_FILE *new;

    if (!src) {
        g_debug(MODULE"%s: File name cannot be NULL", __func__);
        return CR_COPY_ERR;
    }

    if (compression == AUTO_DETECT_COMPRESSION ||
        compression == UNKNOWN_COMPRESSION) {
        g_debug(MODULE"%s: Bad compression type", __func__);
        return CR_COPY_ERR;
    }

    // Src must be a file NOT a directory
    if (g_str_has_suffix(src, "/") ||
        !g_file_test(src, (G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))) 
    {
        g_debug(MODULE"%s: Source (%s) must be directory!", __func__, src);
        return CR_COPY_ERR;
    }

    gchar *dst = (gchar *) in_dst;
    if (!dst) {
        // If destination is NULL, use src + compression suffix
        const gchar *suffix = get_suffix(compression);
        dst = g_strconcat(src, suffix, NULL);
    } else {
        // If destination is dir use filename from src + compression suffix
        if (g_str_has_suffix(in_dst, "/")) {
            const gchar *suffix = get_suffix(compression);
            dst = g_strconcat(in_dst, get_filename(src), suffix, NULL);
        }
    }

    if ((orig = fopen(src, "r")) == NULL) {
        g_debug(MODULE"%s: Cannot open source file %s (%s)", __func__, src, strerror(errno));
        return CR_COPY_ERR;
    }

    if ((new = cw_open(dst, CW_MODE_WRITE, compression)) == NULL) {
        g_debug(MODULE"%s: Cannot open destination file %s", __func__, dst);
        fclose(orig);
        return CR_COPY_ERR;
    }

    while ((readed = fread(buf, 1, BUFFER_SIZE, orig)) > 0) {
        if (cw_write(new, buf, readed) != readed) {
            g_debug(MODULE"%s: Error while copy %s -> %s", __func__, src, dst);
            cw_close(new);
            fclose(orig);
            return CR_COPY_ERR;
        }

        if (readed != BUFFER_SIZE && ferror(orig)) {
            g_debug(MODULE"%s: Error while copy %s -> %s (%s)", __func__, src, dst, strerror(errno));
            cw_close(new);
            fclose(orig);
            return CR_COPY_ERR;
        }
    }

    if (dst != in_dst) {
        g_free(dst);
    }

    cw_close(new);
    fclose(orig);

    return CR_COPY_OK;
}



void download(CURL *handle, const char *url, const char *in_dst, char **error)
{
    CURLcode rcode;
    FILE *file = NULL;


    // If destination is dir use filename from src

    gchar *dst = NULL;
    if (g_str_has_suffix(in_dst, "/")) {
        dst = g_strconcat(in_dst, get_filename(url), NULL);
    } else if (g_file_test(in_dst, (G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))) {
        dst = g_strconcat(in_dst, "/", get_filename(url), NULL);
    } else {
        dst = g_strdup(in_dst);
    }

    file = fopen(dst, "w");
    if (!file) {
        *error = g_strdup_printf(MODULE"%s: Cannot open %s", __func__, dst);
        remove(dst);
        g_free(dst);
        return;
    }


    // Set URL

    if (curl_easy_setopt(handle, CURLOPT_URL, url) != CURLE_OK) {
        *error = g_strdup_printf(MODULE"%s: curl_easy_setopt(CURLOPT_URL) error", __func__);
        fclose(file);
        remove(dst);
        g_free(dst);
        return;
    }


    // Set output file descriptor

    if (curl_easy_setopt(handle, CURLOPT_WRITEDATA, file) != CURLE_OK) {
        *error = g_strdup_printf(MODULE"%s: curl_easy_setopt(CURLOPT_WRITEDATA) error", __func__);
        fclose(file);
        remove(dst);
        g_free(dst);
        return;
    }

    rcode = curl_easy_perform(handle);
    if (rcode != 0) {
        *error = g_strdup_printf(MODULE"%s: curl_easy_perform() error: %s", __func__, curl_easy_strerror(rcode));
        fclose(file);
        remove(dst);
        g_free(dst);
        return;
    }


    g_debug(MODULE"%s: Successfully downloaded: %s", __func__, dst);

    fclose(file);
    g_free(dst);
}



int better_copy_file(const char *src, const char *in_dst)
{
    if (!strstr(src, "://")) {
        // Probably local path
        return copy_file(src, in_dst);
    }

    char *error = NULL;
    CURL *handle = curl_easy_init();
    download(handle, src, in_dst, &error);
    curl_easy_cleanup(handle);
    if (error) {
        g_debug(MODULE"%s: Error while downloading %s: %s", __func__, src, error);
        return CR_COPY_ERR;
    }

    return CR_COPY_OK;
}


int remove_dir_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    UNUSED(sb);
    UNUSED(typeflag);
    UNUSED(ftwbuf);
    int rv = remove(fpath);
    if (rv)
        g_warning("%s: Cannot remove: %s: %s", __func__, fpath, strerror(errno));

    return rv;
}


int remove_dir(const char *path)
{
    return nftw(path, remove_dir_cb, 64, FTW_DEPTH | FTW_PHYS);
}


// Return path with exactly one trailing '/'
char *normalize_dir_path(const char *path)
{
    char *normalized = NULL;

    if (!path)
        return normalized;

    int i = strlen(path);
    if (i == 0) {
        return g_strdup("./");
    }

    do { // Skip all trailing '/'
        i--;
    } while (i >= 0 && path[i] == '/');

    normalized = g_strndup(path, i+2);
    if (normalized[i+1] != '/') {
        normalized[i+1] = '/';
    }

    return normalized;
}


struct Version str_to_version(const char *str)
{
    char *endptr;
    const char *ptr = str;
    struct Version ver;
    ver.version = 0;
    ver.release = 0;
    ver.patch   = 0;
    ver.suffix  = NULL;

    if (!str || str[0] == '\0') {
        return ver;
    }


    // Version chunk

    ver.version = strtol(ptr, &endptr, 10);
    if (!endptr || endptr[0] == '\0') {
        // Whole string has been converted successfully
        return ver;
    } else {
        if (endptr[0] == '.') {
            // '.' is supposed to be delimiter -> skip it and go to next chunk
            ptr = endptr+1;
        } else {
            ver.suffix = g_strdup(endptr);
            return ver;
        }
    }


    // Release chunk

    ver.release = strtol(ptr, &endptr, 10);
    if (!endptr || endptr[0] == '\0') {
        // Whole string has been converted successfully
        return ver;
    } else {
        if (endptr[0] == '.') {
            // '.' is supposed to be delimiter -> skip it and go to next chunk
            ptr = endptr+1;
        } else {
            ver.suffix = g_strdup(endptr);
            return ver;
        }
    }


    // Patch chunk

    ver.patch = strtol(ptr, &endptr, 10);
    if (!endptr || endptr[0] == '\0') {
        // Whole string has been converted successfully
        return ver;
    } else {
        if (endptr[0] == '.') {
            // '.' is supposed to be delimiter -> skip it and go to next chunk
            ptr = endptr+1;
        } else {
            ver.suffix = g_strdup(endptr);
            return ver;
        }
    }

    return ver;
}


// Return values:
// 0 - versions are same
// 1 - first string is bigger version
// 2 - second string is bigger version
int cmp_version_string(const char* str1, const char *str2)
{
    struct Version ver1, ver2;

    if (!str1 && !str2) {
        return 0;
    }

    // Get version
    ver1 = str_to_version(str1);
    ver2 = str_to_version(str2);

    if (ver1.version > ver2.version) {
        return 1;
    } else if (ver1.version < ver2.version) {
        return 2;
    } else if (ver1.release > ver2.release) {
        return 1;
    } else if (ver1.release < ver2.release) {
        return 2;
    } else if (ver1.patch > ver2. patch) {
        return 1;
    } else if (ver1.patch < ver2.patch) {
        return 2;
    }

    int strcmp_res = g_strcmp0(ver1.suffix, ver2.suffix);
    if (strcmp_res > 0) {
        return 1;
    } else if (strcmp_res < 0) {
        return 2;
    }

    g_free(ver1.suffix);
    g_free(ver2.suffix);

    return 0;
}
