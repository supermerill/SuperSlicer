/*
 * Copyright (C) 2013 KLab Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Links related to EXIF, TIFF 6.0 and MPO (Multi Picture Object) format
//
// https://www.exif.org/Exif2-2.PDF
// https://www.itu.int/itudoc/itu-t/com16/tiff-fx/docs/tiff6.pdf
//
// https://en.wikipedia.org/wiki/JPEG#JPEG_Multi-Picture_Format
// http://www.cmsoft.com.br/downloads/cmsoft-stereoscopic-picture-editor-converter/3d-picture-gallery/
// https://dmitrybrant.com/2011/02/08/the-fujifilm-mpo-3d-photo-format
//
#ifdef _MSC_VER
#include <windows.h>
//#define vsnprintf _vsnprintf
#endif

#include "exif.h"

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <memory.h>
#include <ctype.h>

#ifdef _MSC_VER
#ifdef __cplusplus
extern "C" {
#endif
#endif


#pragma pack(2)

#define VERSION  "1.0.1"

#define APP0_MARKER        0xFFE0
#define APP1_MARKER        0xFFE1
#define APP2_MARKER        0xFFE2

// TIFF Header
typedef struct _tiff_Header {
    uint16_t byteOrder;
    uint16_t reserved;
    unsigned int Ifd0thOffset;
} TIFF_HEADER;

// APP1 Exif Segment Header
typedef struct _App_Header {
    uint16_t marker;
    uint16_t length;
    char id[6]; // "Exif\0\0"
    TIFF_HEADER tiff;
} APP_HEADER;

// MPF Segment Header
typedef struct _MPF_Header {
    uint16_t marker;
    uint16_t length;
    char id[4]; // "MPF\0"
    TIFF_HEADER tiff;
} MPF_HEADER;

// tag field in IFD
typedef struct {
    uint16_t tag;
    uint16_t type;
    unsigned int count;
    unsigned int offset;
} IFD_TAG;

// tag node - internal use
typedef struct _tagNode TagNode;
struct _tagNode {
    uint16_t tagId;
    uint16_t type;
    unsigned int count;
    unsigned int *numData;
    uint8_t *byteData;
    uint16_t error;
    TagNode *prev;
    TagNode *next;
};

// IFD table - internal use
typedef struct _ifdTable IfdTable;
struct _ifdTable {
    EXIF_IFD_TYPE ifdType;
    uint16_t tagCount;
    TagNode *tags;
    unsigned int nextIfdOffset;
    uint16_t offset;
    uint16_t length;
    uint8_t *p;
};

static int init(FILE*);
static int systemIsLittleEndian();
static int dataIsLittleEndian();
static void freeIfdTable(void*);
static void *parseIFD(FILE*, unsigned int, unsigned int, EXIF_IFD_TYPE);
static TagNode *getTagNodePtrFromIfd(IfdTable*, uint16_t);
static TagNode *duplicateTagNode(TagNode*);
static void freeTagNode(void*);
static const char *getTagName(int, uint16_t);
static int countIfdTableOnIfdTableArray(void **ifdTableArray);
static IfdTable *getIfdTableFromIfdTableArray(void **ifdTableArray, EXIF_IFD_TYPE ifdType);
static void *createIfdTable(EXIF_IFD_TYPE IfdType, uint16_t tagCount, unsigned int nextOfs);
static void *addTagNodeToIfd(void *pIfd, uint16_t tagId, uint16_t type,
                      unsigned int count, unsigned int *numData,uint8_t *byteData);
static int writeExifSegment(FILE *fp, void **ifdTableArray);
static int removeTagOnIfd(void *pIfd, uint16_t tagId);
static int fixLengthAndOffsetInIfdTables(void **ifdTableArray);
static int setSingleNumDataToTag(TagNode *tag, unsigned int value);
static int getAppNStartOffset(FILE *fp, uint16_t appMarkerN, const char *App1IDString,
                              size_t App1IDStringLength, int *pDQTOffset);
static uint16_t swab16(uint16_t us);
static void PRINTF(char **ms, const char *fmt, ...);
static void _dumpIfdTable(void *pIfd, char **p);

static int Verbose = 0;
static int App1StartOffset = -1;
static int App2StartOffset = -1;
static int MPFStartOffset = -1;
static int JpegDQTOffset = -1;
static APP_HEADER App1Header;
static APP_HEADER App2Header;
static MPF_HEADER MPFHeader;


// private functions

static int dataIsLittleEndian()
{
    return (App1Header.tiff.byteOrder == 0x4949) ? 1 : 0;
}

static int systemIsLittleEndian()
{
    static int i = 1;
    return (int)(*(char*)&i);
}

static uint16_t swab16(uint16_t us)
{
    return (us << 8) | ((us >> 8) & 0x00FF);
}

static unsigned int swab32(unsigned int ui)
{
    return
        ((ui << 24) & 0xFF000000) | ((ui << 8) & 0x00FF0000) |
        ((ui >> 8) & 0x0000FF00) | ((ui >> 24) & 0x000000FF);
}

static uint16_t fix_short(uint16_t us)
{
    return (dataIsLittleEndian() !=
        systemIsLittleEndian()) ? swab16(us) : us;
}

static unsigned int fix_int(unsigned int ui)
{
    return (dataIsLittleEndian() !=
        systemIsLittleEndian()) ? swab32(ui) : ui;
}

// public funtions

/**
 * setVerbose()
 *
 * Verbose output on/off
 *
 * parameters
 *  [in] v : 1=on  0=off
 */
EXIF_API_EXPORT void exif_setVerbose(int v)
{
    Verbose = v;
}

/**
 * removeExifSegmentFromJPEGFile()
 *
 * Remove the Exif segment from a JPEG file
 *
 * parameters
 *  [in] inJPEGFileName : original JPEG file
 *  [in] outJPGEFileName : output JPEG file
 *
 * return
 *   1: OK
 *   0: the Exif segment is not found
 *  -n: error
 *      EXIF_ERR_READ_FILE
 *      EXIF_ERR_WRITE_FILE
 *      EXIF_ERR_INVALID_JPEG
 *      EXIF_ERR_INVALID_APP1HEADER
 */
int exif_removeExifSegmentFromJPEGFile(const char *inJPEGFileName,
                                  const char *outJPGEFileName)
{
    int ofs;
    int i, sts = 1;
    size_t readLen, writeLen;
    uint8_t buf[8192], *p;
    FILE *fpr = NULL, *fpw = NULL;

    fpr = fopen(inJPEGFileName, "rb");
    if (!fpr) {
        sts = EXIF_ERR_READ_FILE;
        goto DONE;
    }
    sts = init(fpr);
    if (sts <= 0) {
        goto DONE;
    }
    fpw = fopen(outJPGEFileName, "wb");
    if (!fpw) {
        sts = EXIF_ERR_WRITE_FILE;
        goto DONE;
    }
    // copy the data in front of the Exif segment
    rewind(fpr);
    p = buf;
    if (App1StartOffset > sizeof(buf)) {
        // allocate new buffer if needed
        p = (uint8_t*)malloc(App1StartOffset);
    }
    if (!p) {
        for (i = 0; i < App1StartOffset; i++) {
            fread(buf, 1, sizeof(char), fpr);
            fwrite(buf, 1, sizeof(char), fpw);
        }
    } else {
        if (fread(p, 1, App1StartOffset, fpr) < (size_t)App1StartOffset) {
            sts = EXIF_ERR_READ_FILE;
            goto DONE;
        }
        if (fwrite(p, 1, App1StartOffset, fpw) < (size_t)App1StartOffset) {
            sts = EXIF_ERR_WRITE_FILE;
            goto DONE;
        }
        if (p != &buf[0]) {
            free(p);
        }
    }
    // seek to the end of the Exif segment
    ofs = App1StartOffset + sizeof(App1Header.marker) + App1Header.length;
    if (fseek(fpr, ofs, SEEK_SET) != 0) {
        sts = EXIF_ERR_READ_FILE;
        goto DONE;
    }
    // read & write
    for (;;) {
        readLen = fread(buf, 1, sizeof(buf), fpr);
        if (readLen <= 0) {
            break;
        }
        writeLen = fwrite(buf, 1, readLen, fpw);
        if (writeLen != readLen) {
            sts = EXIF_ERR_WRITE_FILE;
            goto DONE;
        }
    }
DONE:
    if (fpw) {
        fclose(fpw);
    }
    if (fpr) {
        fclose(fpr);
    }
    return sts;
}

/**
 * fillIfdTableArray()
 *
 * Parse the JPEG header and fill in the IFD table
 *
 * parameters
 *  [in] JPEGFileName : target JPEG file
 *  [out] ifdArray[32] : array of IfdTable pointers
 *
 * return
 *   n: number of IFD tables
 *   0: the Exif segment is not found
 *  -n: error
 *      EXIF_ERR_READ_FILE
 *      EXIF_ERR_INVALID_JPEG
 *      EXIF_ERR_INVALID_APP1HEADER
 *      EXIF_ERR_INVALID_IFD
 */
int EXIF_API_EXPORT exif_fillIfdTableArray(const char *JPEGFileName, void* ifdArray[32])
{
    #define FMT_ERR "critical error in %s IFD\n"

    int sts = 1, ifdCount = 0;
    unsigned int ifdOffset;
    FILE *fp = NULL;
    TagNode *tag;
    IfdTable *IFD_0th, *IFD_exif, *IFD_gps, *IFD_io, *IFD_1st, *mpf_ifd;

    IFD_0th = IFD_exif = IFD_gps = IFD_io = IFD_1st = NULL;
    memset(ifdArray, 0, sizeof(void*) * 32);

    fp = fopen(JPEGFileName, "rb");
    if (!fp) {
        sts = EXIF_ERR_READ_FILE;
        goto DONE;
    }
    sts = init(fp);
    if (sts <= 0) {
        goto DONE;
    }
    if (Verbose) {
        printf("system: %s-endian\n  data: %s-endian\n",
            systemIsLittleEndian() ? "little" : "big",
            dataIsLittleEndian() ? "little" : "big");
    }

    // for 0th IFD
    IFD_0th = parseIFD(fp, App1StartOffset + offsetof(APP_HEADER, tiff), App1Header.tiff.Ifd0thOffset, IFD_0TH);
    if (!IFD_0th) {
        if (Verbose) {
            printf(FMT_ERR, "0th");
        }
        sts = EXIF_ERR_INVALID_IFD;
        goto DONE; // non-continuable
    }
    ifdArray[ifdCount++] = IFD_0th;

    if (MPFStartOffset > 0) {
        mpf_ifd = parseIFD(fp, MPFStartOffset + offsetof(MPF_HEADER, tiff), MPFHeader.tiff.Ifd0thOffset, IFD_MPF);
        ifdArray[ifdCount++] = mpf_ifd;
    }

    // for Exif IFD
    tag = getTagNodePtrFromIfd(IFD_0th, TAG_ExifIFDPointer);
    if (tag && !tag->error) {
        ifdOffset = tag->numData[0];
        if (ifdOffset != 0) {
            IFD_exif = parseIFD(fp, App1StartOffset + offsetof(APP_HEADER, tiff), ifdOffset, IFD_EXIF);
            if (IFD_exif) {
                ifdArray[ifdCount++] = IFD_exif;
                // for InteroperabilityIFDPointer IFD
                tag = getTagNodePtrFromIfd(IFD_exif, TAG_InteroperabilityIFDPointer);
                if (tag && !tag->error) {
                    ifdOffset = tag->numData[0];
                    if (ifdOffset != 0) {
                        IFD_io = parseIFD(fp, App1StartOffset + offsetof(APP_HEADER, tiff), ifdOffset, IFD_IO);
                        if (IFD_io) {
                            ifdArray[ifdCount++] = IFD_io;
                        } else {
                            if (Verbose) {
                                printf(FMT_ERR, "Interoperability");
                            }
                            sts = EXIF_ERR_INVALID_IFD;
                        }
                    }
                }
            } else {
                if (Verbose) {
                    printf(FMT_ERR, "Exif");
                }
                sts = EXIF_ERR_INVALID_IFD;
            }
        }
    }

    // for GPS IFD
    tag = getTagNodePtrFromIfd(IFD_0th, TAG_GPSInfoIFDPointer);
    if (tag && !tag->error) {
        ifdOffset = tag->numData[0];
        if (ifdOffset != 0) {
            IFD_gps = parseIFD(fp, App1StartOffset + offsetof(APP_HEADER, tiff), ifdOffset, IFD_GPS);
            if (IFD_gps) {
                ifdArray[ifdCount++] = IFD_gps;
            } else {
                if (Verbose) {
                    printf(FMT_ERR, "GPS");
                }
                sts = EXIF_ERR_INVALID_IFD;
            }
        }
    }

    // for 1st IFD
    ifdOffset = IFD_0th->nextIfdOffset;
    if (Verbose) {
        printf("1st IFD ifdOffset=%u\n", ifdOffset);
    }
    if (ifdOffset != 0) {
        IFD_1st = parseIFD(fp, App1StartOffset + offsetof(APP_HEADER, tiff), ifdOffset, IFD_1ST);
        if (IFD_1st) {
            ifdArray[ifdCount++] = IFD_1st;
        } else {
            if (Verbose) {
                printf(FMT_ERR, "1st");
            }
            sts = EXIF_ERR_INVALID_IFD;
        }
    }

DONE:
    if (fp) {
        fclose(fp);
    }
    return (sts <= 0) ? sts : ifdCount;
}

/**
 * createIfdTableArray()
 *
 * Parse the JPEG header and create the pointer array of the IFD tables
 *
 * parameters
 *  [in] JPEGFileName : target JPEG file
 *  [out] result : result status value
 *   n: number of IFD tables
 *   0: the Exif segment is not found
 *  -n: error
 *      EXIF_ERR_READ_FILE
 *      EXIF_ERR_INVALID_JPEG
 *      EXIF_ERR_INVALID_APP1HEADER
 *      EXIF_ERR_INVALID_IFD
 *
 * return
 *   NULL: error or no Exif segment
 *  !NULL: pointer array of the IFD tables
 */
void EXIF_API_EXPORT ** EXIF_API_CALL exif_createIfdTableArray(const char *JPEGFileName, int *result)
{
    void* ifdTable[32];
    void** ppIfdArray = NULL;
    int i, count = exif_fillIfdTableArray(JPEGFileName, ifdTable);
    *result = count;
    if (count > 0) {
        // +1 extra NULL element to the array
        ppIfdArray = (void**)malloc(sizeof(void*)*(count+1));
        memset(ppIfdArray, 0, sizeof(void*)*(count+1));
        for (i = 0; ifdTable[i] != NULL; i++) {
            ppIfdArray[i] = ifdTable[i];
        }
    }
    return ppIfdArray;
}

/**
 * freeIfdTables()
 *
 * Free the pointer array of the IFD tables
 *
 * parameters
 *  [in] ifdArray : address of the IFD array
 */
void EXIF_API_EXPORT exif_freeIfdTables(void* ifdArray[32])
{
    int i;
    for (i = 0; ifdArray[i] != NULL; i++) {
        freeIfdTable(ifdArray[i]);
    }
}

/**
 * freeIfdTableArray()
 *
 * Free the pointer array of the IFD tables
 *
 * parameters
 *  [in] ifdArray : address of the IFD array
 */
void EXIF_API_EXPORT exif_freeIfdTableArray(void **ifdArray)
{
    exif_freeIfdTables(ifdArray);
    free(ifdArray);
}

/**
 * getIfdType()
 *
 * Returns the type of the IFD
 *
 * parameters
 *  [in] ifd: target IFD
 *
 * return
 *  IFD TYPE value
 */
EXIF_IFD_TYPE EXIF_API_EXPORT exif_getIfdType(void *pIfd)
{
    IfdTable *ifd = (IfdTable*)pIfd;
    if (!ifd) {
        return IFD_UNKNOWN;
    }
    return ifd->ifdType;
}

/**
 * dumpIfdTable()
 *
 * Dump the IFD table
 *
 * parameters
 *  [in] ifd: target IFD
 */

void EXIF_API_EXPORT exif_dumpIfdTable(void *pIfd)
{
    _dumpIfdTable(pIfd, NULL);
}

void EXIF_API_EXPORT exif_getIfdTableDump(void *pIfd, char **pp)
{
    if (pp) {
        *pp = NULL;
    }
    _dumpIfdTable(pIfd, pp);
}

static void _dumpIfdTable(void *pIfd, char **p)
{
    int i;
    IfdTable *ifd;
    TagNode *tag;
    char tagName[512];
    int cnt = 0;
    unsigned int count;

    if (!pIfd) {
        return;
    }
    ifd = (IfdTable*)pIfd;

    PRINTF(p, "\n{%s IFD}",
        (ifd->ifdType == IFD_0TH)  ? "0TH" :
        (ifd->ifdType == IFD_1ST)  ? "1ST" :
        (ifd->ifdType == IFD_EXIF) ? "EXIF" :
        (ifd->ifdType == IFD_GPS)  ? "GPS" :
        (ifd->ifdType == IFD_IO)   ? "Interoperability" :
        (ifd->ifdType == IFD_MPF)  ? "MPF" : "");

    if (Verbose) {
        PRINTF(p, " tags=%u\n", ifd->tagCount);
    } else {
        PRINTF(p, "\n");
    }

    tag = ifd->tags;
    while (tag) {
        if (Verbose) {
            PRINTF(p, "tag[%02d] 0x%04X %s\n",
                cnt++, tag->tagId, getTagName(ifd->ifdType, tag->tagId));
            PRINTF(p, "\ttype=%u count=%u ", tag->type, tag->count);
            PRINTF(p, "val=");
        } else {
            strcpy(tagName, getTagName(ifd->ifdType, tag->tagId));
            PRINTF(p, " - %s: ", (strlen(tagName) > 0) ? tagName : "(unknown)");
        }
        if (tag->error) {
            PRINTF(p, "(error)");
        } else {
            switch (tag->type) {
            case TYPE_BYTE:
                for (i = 0; i < (int)tag->count; i++) {
                    PRINTF(p, "%u ", (uint8_t)tag->numData[i]);
                }
                break;

            case TYPE_ASCII:
                PRINTF(p, "[%s]", (char*)tag->byteData);
                break;

            case TYPE_SHORT:
                for (i = 0; i < (int)tag->count; i++) {
                    PRINTF(p, "%hu ", (uint16_t)tag->numData[i]);
                }
                break;

            case TYPE_LONG:
                for (i = 0; i < (int)tag->count; i++) {
                    PRINTF(p, "%u ", tag->numData[i]);
                }
                break;

            case TYPE_RATIONAL:
                for (i = 0; i < (int)tag->count; i++) {
                    PRINTF(p, "%u/%u ", tag->numData[i*2], tag->numData[i*2+1]);
                }
                break;

            case TYPE_SBYTE:
                for (i = 0; i < (int)tag->count; i++) {
                    PRINTF(p, "%d ", (char)tag->numData[i]);
                }
                break;

            case TYPE_UNDEFINED:
                count = tag->count;
                // omit too long data if !Verbose
                if (count > 16 && !Verbose) {
                    count = 16;
                }
                for (i = 0; i < (int)count; i++) {
                    // if character is printable
                    if (isgraph(tag->byteData[i])) {
                        PRINTF(p, "%c ", tag->byteData[i]);
                    } else {
                        PRINTF(p, "0x%02x ", tag->byteData[i]);
                    }
                }
                if (count < tag->count) {
                    PRINTF(p, "(omitted)");
                }
                break;

            case TYPE_SSHORT:
                for (i = 0; i < (int)tag->count; i++) {
                    PRINTF(p, "%hd ", (short)tag->numData[i]);
                }
                break;

            case TYPE_SLONG:
                for (i = 0; i < (int)tag->count; i++) {
                    PRINTF(p, "%d ", (int)tag->numData[i]);
                }
                break;

            case TYPE_SRATIONAL:
                for (i = 0; i < (int)tag->count; i++) {
                    PRINTF(p, "%d/%d ", (int)tag->numData[i*2], (int)tag->numData[i*2+1]);
                }
                break;

            default:
                break;
            }
        }
        PRINTF(p, "\n");

        tag = tag->next;
    }
    return;
}

/**
 * dumpIfdTableArray()
 *
 * Dump the array of the IFD tables
 *
 * parameters
 *  [in] ifdArray : address of the IFD array
 */
void EXIF_API_EXPORT exif_dumpIfdTableArray(void **ifdArray)
{
    int i;
    if (ifdArray) {
        for (i = 0; ifdArray[i] != NULL; i++) {
            exif_dumpIfdTable(ifdArray[i]);
        }
    }
}

/**
 * getTagInfo()
 *
 * Get the ExifTagNodeInfo that matches the EXIF_IFD_TYPE & TagId
 *
 * parameters
 *  [in] ifdArray : address of the IFD array
 *  [in] ifdType : target IFD TYPE
 *  [in] tagId : target tag ID
 *
 * return
 *   NULL: tag is not found
 *  !NULL: address of the ExifTagNodeInfo structure
 */
ExifTagNodeInfo EXIF_API_EXPORT * exif_getTagInfo(void **ifdArray,
                       EXIF_IFD_TYPE ifdType,
                       uint16_t tagId)
{
    int i;
    if (!ifdArray) {
        return NULL;
    }
    for (i = 0; ifdArray[i] != NULL; i++) {
        if (exif_getIfdType(ifdArray[i]) == ifdType) {
            void *targetTag = getTagNodePtrFromIfd(ifdArray[i], tagId);
            if (!targetTag) {
                return NULL;
            }
            return (ExifTagNodeInfo*)duplicateTagNode(targetTag);
        }
    }
    return NULL;
}

/**
 * getTagInfoFromIfd()
 *
 * Get the ExifTagNodeInfo that matches the TagId
 *
 * parameters
 *  [in] ifd : target IFD table
 *  [in] tagId : target tag ID
 *
 * return
 *  NULL: tag is not found
 *  !NULL: address of the ExifTagNodeInfo structure
 */
ExifTagNodeInfo EXIF_API_EXPORT * exif_getTagInfoFromIfd(void *ifd,
                               uint16_t tagId)
{
    if (!ifd) {
        return NULL;
    }
    return (ExifTagNodeInfo*)getTagNodePtrFromIfd(ifd, tagId);
}

/**
 * freeTagInfo()
 *
 * Free the ExifTagNodeInfo allocated by getTagInfo() or getTagInfoFromIfd()
 *
 * parameters
 *  [in] tag : target ExifTagNodeInfo
 */
void EXIF_API_EXPORT exif_freeTagInfo(void *tag)
{
    freeTagNode(tag);
}

/**
 * queryTagNodeIsExist()
 *
 * Query if the specified tag node is exist in the IFD tables
 *
 * parameters
 *  [in] ifdTableArray: address of the IFD tables array
 *  [in] ifdType : target IFD type
 *  [in] tagId : target tag ID
 *
 * return
 *  0: not exist
 *  1: exist
 */
int EXIF_API_EXPORT exif_queryTagNodeIsExist(void **ifdTableArray,
                        EXIF_IFD_TYPE ifdType,
                        uint16_t tagId)
{
    IfdTable *ifd;
    TagNode *tag;
    if (!ifdTableArray) {
        return 0;
    }
    ifd = getIfdTableFromIfdTableArray(ifdTableArray, ifdType);
    if (!ifd) {
        return 0;
    }
    tag = getTagNodePtrFromIfd(ifd, tagId);
    if (!tag) {
        return 0;
    }
    return 1;
}

/**
 * createTagInfo()
 *
 * Create new ExifTagNodeInfo block
 *
 * parameters
 *  [in] tagId: id of the tag
 *  [in] type: type of the tag
 *  [in] count: data count of the tag
 *  [out] pResult : error status
 *   0: OK
 *  -n: error
 *      EXIF_ERR_INVALID_TYPE
 *      EXIF_ERR_INVALID_COUNT
 *      EXIF_ERR_MEMALLOC
 *
 * return
 *  NULL: error
 * !NULL: address of the newly created ExifTagNodeInfo
 */
ExifTagNodeInfo EXIF_API_EXPORT * exif_createTagInfo(uint16_t tagId,
                           uint16_t type,
                           unsigned int count,
                           int *pResult)
{
    TagNode *tag;
    if (type < TYPE_BYTE || type > TYPE_SRATIONAL) {
        if (pResult) {
            *pResult = EXIF_ERR_INVALID_TYPE;
        }
        return NULL;
    }
    if (count <= 0) {
        if (pResult) {
            *pResult = EXIF_ERR_INVALID_COUNT;
        }
        return NULL;
    }
    tag = (TagNode*)malloc(sizeof(TagNode));
    if (!tag) {
        if (pResult) {
            *pResult = EXIF_ERR_MEMALLOC;
        }
        return NULL;
    }
    memset(tag, 0, sizeof(TagNode));
    tag->tagId = tagId;
    tag->type = type;
    tag->count = count;

    if (type == TYPE_ASCII || type == TYPE_UNDEFINED) {
        tag->byteData = (uint8_t*)malloc(count*sizeof(char));
    }
    else if (type == TYPE_BYTE   ||
             type == TYPE_SBYTE  ||
             type == TYPE_SHORT  ||
             type == TYPE_LONG   ||
             type == TYPE_SSHORT ||
             type == TYPE_SLONG) {
        tag->numData = (unsigned int*)malloc(count*sizeof(int));
    }
    else if (type == TYPE_RATIONAL ||
             type == TYPE_SRATIONAL) {
        tag->numData = (unsigned int*)malloc(count*sizeof(int)*2);
    }
    if (pResult) {
        *pResult = 0;
    }
    return (ExifTagNodeInfo*)tag;
}

/**
 * removeIfdTableFromIfdTableArray()
 *
 * Remove the IFD table from the ifdTableArray
 *
 * parameters
 *  [in] ifdTableArray: address of the IFD tables array
 *  [in] ifdType : target IFD type
 *
 * return
 *  n: number of the removed IFD tables
 */
int EXIF_API_EXPORT exif_removeIfdTableFromIfdTableArray(void **ifdTableArray, EXIF_IFD_TYPE ifdType)
{
    int i, num = 0, ret = 0;
    if (!ifdTableArray) {
        return 0;
    }
    // count IFD tables
    num = countIfdTableOnIfdTableArray(ifdTableArray);
    for (;;) { // possibility of multiple entries
        for (i = 0; i < num; i++) {
            IfdTable *ifd = ifdTableArray[i];
            if (ifd->ifdType == ifdType) {
                freeIfdTable(ifd);
                ifdTableArray[i] = NULL;
                ret++;
                break;
            }
        }
        if (i == num) {
            break; // no more found
        }
        // left justify the array
        memcpy(&ifdTableArray[i], &ifdTableArray[i+1], (num-i) * sizeof(void*));
        num--;
    }
    return ret;
}

/**
 * insertIfdTableToIfdTableArray()
 *
 * Insert new IFD table to the ifdTableArray
 *
 * parameters
 *  [in] ifdTableArray: address of the IFD tables array
 *  [in] ifdType : target IFD type
 *  [out] pResult : error status
 *   0: OK
 *  -n: error
 *      EXIF_ERR_ALREADY_EXIST
 *      EXIF_ERR_MEMALLOC
 *
 * return
 *  NULL: error
 * !NULL: address of the newly created ifdTableArray
 *
 * note
 * This function frees old ifdTableArray if is not NULL.
 */
void EXIF_API_EXPORT ** exif_insertIfdTableToIfdTableArray(void **ifdTableArray,
                                     EXIF_IFD_TYPE ifdType,
                                     int *pResult)
{
    void *newIfd;
    void **newIfdTableArray;
    int num = 0;
    if (!ifdTableArray) {
        num = 0;
    } else {
        num = countIfdTableOnIfdTableArray(ifdTableArray);
    }
    if (num > 0 && getIfdTableFromIfdTableArray(ifdTableArray, ifdType) != NULL) {
        if (pResult) {
            *pResult = EXIF_ERR_ALREADY_EXIST;
        }
        return NULL;
    }
    // create the new IFD table
    newIfd = createIfdTable(ifdType, 0, 0);
    if (!newIfd) {
        if (pResult) {
            *pResult = EXIF_ERR_MEMALLOC;
        }
        return NULL;
    }
    // copy existing IFD tables to the new array
    newIfdTableArray = (void**)malloc(sizeof(void*)*(num+2));
    if (!newIfdTableArray) {
        if (pResult) {
            *pResult = EXIF_ERR_MEMALLOC;
        }
        free(newIfd);
        return NULL;
    }
    memset(newIfdTableArray, 0, sizeof(void*)*(num+2));
    if (num > 0) {
        memcpy(newIfdTableArray, ifdTableArray, num * sizeof(void*));
    }
    // add the new IFD table
    newIfdTableArray[num] = newIfd;
    if (ifdTableArray) {
        free(ifdTableArray); // free the old array
    }
    if (pResult) {
        *pResult = 0;
    }
    return newIfdTableArray;
}

/**
 * removeTagNodeFromIfdTableArray()
 *
 * Remove the specified tag node from the IFD table
 *
 * parameters
 *  [in] ifdTableArray: address of the IFD tables array
 *  [in] ifdType : target IFD type
 *  [in] tagId : target tag ID
 *
 * return
 *  n: number of the removed tags
 */
int EXIF_API_EXPORT exif_removeTagNodeFromIfdTableArray(void **ifdTableArray,
                             EXIF_IFD_TYPE ifdType,
                             uint16_t tagId)
{
    IfdTable *ifd = getIfdTableFromIfdTableArray(ifdTableArray, ifdType);
    if (!ifd) {
        return 0;
    }
    return removeTagOnIfd(ifd, tagId);
}

/**
 * insertTagNodeToIfdTableArray()
 *
 * Insert the specified tag node to the IFD table
 *
 * parameters
 *  [in] ifdTableArray: address of the IFD tables array
 *  [in] ifdType : target IFD type
 *  [in] ExifTagNodeInfo: address of the ExifTagNodeInfo
 *
 * note
 * This function uses the copy of the specified tag data.
 * The caller must free it after this function returns.
 *
 * return
 *  0: OK
 *  EXIF_ERR_INVALID_POINTER:
 *  EXIF_ERR_NOT_EXIST:
 *  EXIF_ERR_ALREADY_EXIST:
 *  EXIF_ERR_UNKNOWN:
 */
int EXIF_API_EXPORT exif_insertTagNodeToIfdTableArray(void **ifdTableArray,
                             EXIF_IFD_TYPE ifdType,
                             ExifTagNodeInfo *ExifTagNodeInfo)
{
    IfdTable *ifd;
    if (!ifdTableArray) {
        return EXIF_ERR_INVALID_POINTER;
    }
    if (!ExifTagNodeInfo) {
        return EXIF_ERR_INVALID_POINTER;
    }
    ifd = getIfdTableFromIfdTableArray(ifdTableArray, ifdType);
    if (!ifd) {
        return EXIF_ERR_NOT_EXIST;
    }
    // already exists the same type entry
    if (getTagNodePtrFromIfd(ifd, ExifTagNodeInfo->tagId) != NULL) {
        return EXIF_ERR_ALREADY_EXIST;
    }
    // add to the IFD table
    if (!addTagNodeToIfd(ifd,
                    ExifTagNodeInfo->tagId,
                    ExifTagNodeInfo->type,
                    ExifTagNodeInfo->count,
                    ExifTagNodeInfo->numData,
                    ExifTagNodeInfo->byteData)) {
        return EXIF_ERR_UNKNOWN;
    }
    ifd->tagCount++;
    return 0;
}

/**
 * getThumbnailDataOnIfdTableArray()
 *
 * Get a copy of the thumbnail data from the 1st IFD table
 *
 * parameters
 *  [in] ifdTableArray : address of the IFD tables array
 *  [out] pLength : returns the length of the thumbnail data
 *  [out] pResult : error status
 *   0: OK
 *  -n: error
 *      EXIF_ERR_INVALID_POINTER
 *      EXIF_ERR_MEMALLOC
 *      EXIF_ERR_NOT_EXIST
 *
 * return
 *  NULL: error
 * !NULL: the thumbnail data
 *
 * note
 * This function returns the copy of the thumbnail data.
 * The caller must free it.
 */
uint8_t EXIF_API_EXPORT * exif_getThumbnailDataOnIfdTableArray(void **ifdTableArray,
                                               unsigned int *pLength,
                                               int *pResult)
{
    IfdTable *ifd;
    TagNode *tag;
    unsigned int len;
    uint8_t *retp;
    if (!ifdTableArray || !pLength) {
        if (pResult) {
            *pResult = EXIF_ERR_INVALID_POINTER;
        }
        return NULL;
    }
    ifd = getIfdTableFromIfdTableArray(ifdTableArray, IFD_1ST);
    if (!ifd || !ifd->p) {
        if (pResult) {
            *pResult = EXIF_ERR_NOT_EXIST;
        }
        return NULL;
    }
    tag = getTagNodePtrFromIfd(ifd, TAG_JPEGInterchangeFormatLength);
    if (!tag || tag->error) {
        if (pResult) {
            *pResult = EXIF_ERR_NOT_EXIST;
        }
        return NULL;
    }
    len = tag->numData[0];
    if (len <= 0) {
        if (pResult) {
            *pResult = EXIF_ERR_NOT_EXIST;
        }
        return NULL;
    }
    retp= (uint8_t*)malloc(len);
    if (!retp) {
        if (pResult) {
            *pResult = EXIF_ERR_MEMALLOC;
        }
        return NULL;
    }
    memcpy(retp, ifd->p, len);
    *pLength = len;
    if (pResult) {
        *pResult = 0;
    }
    return retp;
}

/**
 * setThumbnailDataOnIfdTableArray()
 *
 * Set or update the thumbnail data to the 1st IFD table
 *
 * parameters
 *  [in] ifdTableArray : address of the IFD tables array
 *  [in] pData : thumbnail data
 *  [in] length : thumbnail data length
 *
 * note
 * This function creates the copy of the specified data.
 * The caller must free it after this function returns.
 *
 * return
 *   0: OK
 *  -n: error
 *      EXIF_ERR_INVALID_POINTER
 *      EXIF_ERR_MEMALLOC
 *      EXIF_ERR_UNKNOWN
 */
int EXIF_API_EXPORT exif_setThumbnailDataOnIfdTableArray(void **ifdTableArray,
                                    uint8_t *pData,
                                    unsigned int length)
{
    IfdTable *ifd;
    TagNode *tag;
    unsigned int zero = 0;
    if (!ifdTableArray || !pData || length <= 0) {
        return EXIF_ERR_INVALID_POINTER;
    }
    ifd = getIfdTableFromIfdTableArray(ifdTableArray, IFD_1ST);
    if (!ifd) {
        int count = countIfdTableOnIfdTableArray(ifdTableArray);
        void* ifd1st = createIfdTable(IFD_1ST, 0, 0);
        printf("count=%d ifd1st=%p\n", count, ifd1st);
        ifdTableArray[count] = ifd1st;
        ifd = getIfdTableFromIfdTableArray(ifdTableArray, IFD_1ST);
        if (!ifd) {
            return EXIF_ERR_NOT_EXIST;
        }
    }
    if (ifd->p) {
        free(ifd->p);
    }
    // set thumbnail length;
    tag = getTagNodePtrFromIfd(ifd, TAG_JPEGInterchangeFormatLength);
    if (tag) {
        setSingleNumDataToTag(tag, length);
    } else {
        if (!addTagNodeToIfd(ifd, TAG_JPEGInterchangeFormatLength,
                            TYPE_LONG, 1, &length, NULL)) {
            return EXIF_ERR_UNKNOWN;
        }
    }
    tag = getTagNodePtrFromIfd(ifd, TAG_JPEGInterchangeFormat);
    if (tag) {
        setSingleNumDataToTag(tag, zero);
    } else {
        // add thumbnail offset tag if not exist
        addTagNodeToIfd(ifd, TAG_JPEGInterchangeFormat,
                            TYPE_LONG, 1, &zero, NULL);
    }
    ifd->p = (uint8_t*)malloc(length);
    if (!ifd->p) {
        return EXIF_ERR_MEMALLOC;
    }
    memcpy(ifd->p, pData, length);
    return 0;
}

/**
 * updateExifSegmentInJPEGFile()
 *
 * Update the Exif segment in a JPEG file
 *
 * parameters
 *  [in] inJPEGFileName : original JPEG file
 *  [in] outJPGEFileName : output JPEG file
 *  [in] ifdTableArray : address of the IFD tables array
 *
 * return
 *   1: OK
 *  -n: error
 *      EXIF_ERR_READ_FILE
 *      EXIF_ERR_WRITE_FILE
 *      EXIF_ERR_INVALID_JPEG
 *      EXIF_ERR_INVALID_APP1HEADER
 *      EXIF_ERR_INVALID_POINTER
 *      ERROR_UNKNOWN:
 */
int EXIF_API_EXPORT exif_updateExifSegmentInJPEGFile(const char *inJPEGFileName,
                                const char *outJPGEFileName,
                                void **ifdTableArray)
{
    int ofs;
    int i, sts = 1, hasExifSegment;
    size_t readLen, writeLen;
    uint8_t buf[8192], *p;
    FILE *fpr = NULL, *fpw = NULL;

    // refresh the length and offset variables in the IFD table
    sts = fixLengthAndOffsetInIfdTables(ifdTableArray);
    if (sts != 0) {
        goto DONE;
    }
    fpr = fopen(inJPEGFileName, "rb");
    if (!fpr) {
        sts = EXIF_ERR_READ_FILE;
        goto DONE;
    }
    sts = init(fpr);
    if (sts < 0) {
        goto DONE;
    }
    if (sts == 0) {
        hasExifSegment = 0;
        ofs = JpegDQTOffset;
    } else {
        hasExifSegment = 1;
        ofs = App1StartOffset;
    }
    fpw = fopen(outJPGEFileName, "wb");
    if (!fpw) {
        sts = EXIF_ERR_WRITE_FILE;
        goto DONE;
    }
    // copy the data in front of the Exif segment
    rewind(fpr);
    p = buf;
    if (ofs > sizeof(buf)) {
        // allocate new buffer if needed
        p = (uint8_t*)malloc(ofs);
    }
    if (!p) {
        for (i = 0; i < ofs; i++) {
            fread(buf, 1, sizeof(char), fpr);
            fwrite(buf, 1, sizeof(char), fpw);
        }
    } else {
        if (fread(p, 1, ofs, fpr) < (size_t)ofs) {
            sts = EXIF_ERR_READ_FILE;
            goto DONE;
        }
        if (fwrite(p, 1, ofs, fpw) < (size_t)ofs) {
            sts = EXIF_ERR_WRITE_FILE;
            goto DONE;
        }
        if (p != &buf[0]) {
            free(p);
        }
    }
    // write new Exif segment
    sts = writeExifSegment(fpw, ifdTableArray);
    if (sts != 0) {
        goto DONE;
    }
    sts = 1;
    if (hasExifSegment) {
        // seek to the end of the Exif segment
        ofs = App1StartOffset + sizeof(App1Header.marker) + App1Header.length;
        if (fseek(fpr, ofs, SEEK_SET) != 0) {
            sts = EXIF_ERR_READ_FILE;
            goto DONE;
        }
    }
    // read & write
    for (;;) {
        readLen = fread(buf, 1, sizeof(buf), fpr);
        if (readLen <= 0) {
            break;
        }
        writeLen = fwrite(buf, 1, readLen, fpw);
        if (writeLen != readLen) {
            sts = EXIF_ERR_WRITE_FILE;
            goto DONE;
        }
    }
DONE:
    if (fpw) {
        fclose(fpw);
    }
    if (fpr) {
        fclose(fpr);
    }
    return sts;
}

/**
 * removeAdobeMetadataSegmentFromJPEGFile()
 *
 * Remove Adobe's XMP metadata segment from a JPEG file
 *
 * parameters
 *  [in] inJPEGFileName : original JPEG file
 *  [in] outJPGEFileName : output JPEG file
 *
 * return
 *   1: OK
 *   0: Adobe's metadata segment is not found
 *  -n: error
 *      EXIF_ERR_READ_FILE
 *      EXIF_ERR_WRITE_FILE
 *      EXIF_ERR_INVALID_JPEG
 *      EXIF_ERR_INVALID_APP1HEADER
 */
int EXIF_API_EXPORT exif_removeAdobeMetadataSegmentFromJPEGFile(const char *inJPEGFileName,
                                           const char *outJPGEFileName)
{
#define ADOBE_METADATA_ID     "http://ns.adobe.com/xap/"
#define ADOBE_METADATA_ID_LEN 24

    typedef struct _SegmentHeader {
        uint16_t marker;
        uint16_t length;
    } SEGMENT_HEADER;

    SEGMENT_HEADER hdr;
    int i, sts = 1;
    size_t readLen, writeLen;
    unsigned int ofs;
    uint8_t buf[8192], *p;
    FILE *fpr = NULL, *fpw = NULL;

    fpr = fopen(inJPEGFileName, "rb");
    if (!fpr) {
        sts = EXIF_ERR_READ_FILE;
        goto DONE;
    }
    sts = getAppNStartOffset(fpr, APP1_MARKER, ADOBE_METADATA_ID, ADOBE_METADATA_ID_LEN, NULL);
    if (sts <= 0) { // target segment is not exist or something error
        goto DONE;
    }
    ofs = sts;
    sts = 1;
    fpw = fopen(outJPGEFileName, "wb");
    if (!fpw) {
        sts = EXIF_ERR_WRITE_FILE;
        goto DONE;
    }
    // copy the data in front of the App1 segment
    rewind(fpr);
    p = buf;
    if (ofs > sizeof(buf)) {
        // allocate new buffer if needed
        p = (uint8_t*)malloc(ofs);
    }
    if (!p) {
        for (i = 0; i < (int)ofs; i++) {
            fread(buf, 1, sizeof(char), fpr);
            fwrite(buf, 1, sizeof(char), fpw);
        }
    } else {
        if (fread(p, 1, ofs, fpr) < (size_t)ofs) {
            sts = EXIF_ERR_READ_FILE;
            goto DONE;
        }
        if (fwrite(p, 1, ofs, fpw) < (size_t)ofs) {
            sts = EXIF_ERR_WRITE_FILE;
            goto DONE;
        }
        if (p != &buf[0]) {
            free(p);
        }
    }
    if (fread(&hdr, 1, sizeof(SEGMENT_HEADER), fpr) != sizeof(SEGMENT_HEADER)) {
        sts = EXIF_ERR_READ_FILE;
        goto DONE;
    }
    if (systemIsLittleEndian()) {
        // the segment length value is always in big-endian order
        hdr.length = swab16(hdr.length);
    }
    // seek to the end of the App1 segment
    if (fseek(fpr, hdr.length - sizeof(hdr.length), SEEK_CUR) != 0) {
        sts = EXIF_ERR_READ_FILE;
        goto DONE;
    }
    // read & write
    for (;;) {
        readLen = fread(buf, 1, sizeof(buf), fpr);
        if (readLen <= 0) {
            break;
        }
        writeLen = fwrite(buf, 1, readLen, fpw);
        if (writeLen != readLen) {
            sts = EXIF_ERR_WRITE_FILE;
            goto DONE;
        }
    }
DONE:
    if (fpw) {
        fclose(fpw);
    }
    if (fpr) {
        fclose(fpr);
    }
    return sts;
}
static int seekToRelativeOffset(FILE *fp, unsigned int baseOff, unsigned int ofs)
{
    return fseek(fp, baseOff + ofs, SEEK_SET);
}

static const char *getTagName(int ifdType, uint16_t tagId)
{
    if (ifdType == IFD_0TH || ifdType == IFD_1ST || ifdType == IFD_EXIF) {
        switch (tagId) {
            case TAG_ImageWidth: return "ImageWidth";
            case TAG_ImageLength: return "ImageLength";
            case TAG_BitsPerSample: return "BitsPerSample";
            case TAG_Compression: return "Compression";
            case TAG_PhotometricInterpretation: return "PhotometricInterpretation";
            case TAG_Orientation: return "Orientation";
            case TAG_SamplesPerPixel: return "SamplesPerPixel";
            case TAG_PlanarConfiguration: return "PlanarConfiguration";
            case TAG_YCbCrSubSampling: return "YCbCrSubSampling";
            case TAG_YCbCrPositioning: return "YCbCrPositioning";
            case TAG_XResolution: return "XResolution";
            case TAG_YResolution: return "YResolution";
            case TAG_ResolutionUnit: return "ResolutionUnit";

            case TAG_StripOffsets: return "StripOffsets";
            case TAG_RowsPerStrip: return "RowsPerStrip";
            case TAG_StripByteCounts: return "StripByteCounts";
            case TAG_JPEGInterchangeFormat: return "JPEGInterchangeFormat";
            case TAG_JPEGInterchangeFormatLength: return "JPEGInterchangeFormatLength";

            case TAG_TransferFunction: return "TransferFunction";
            case TAG_WhitePoint: return "WhitePoint";
            case TAG_PrimaryChromaticities: return "PrimaryChromaticities";
            case TAG_YCbCrCoefficients: return "YCbCrCoefficients";
            case TAG_ReferenceBlackWhite: return "ReferenceBlackWhite";

            case TAG_DateTime: return "DateTime";
            case TAG_ImageDescription: return "ImageDescription";
            case TAG_Make: return "Make";
            case TAG_Model: return "Model";
            case TAG_Software: return "Software";
            case TAG_Artist: return "Artist";
            case TAG_Copyright: return "Copyright";
            case TAG_ExifIFDPointer: return "ExifIFDPointer";
            case TAG_GPSInfoIFDPointer: return "GPSInfoIFDPointer";
            case TAG_InteroperabilityIFDPointer: return "InteroperabilityIFDPointer";

            case TAG_Rating: return "Rating";

            case TAG_ExifVersion: return "ExifVersion";
            case TAG_FlashPixVersion: return "FlashPixVersion";

            case TAG_ColorSpace: return "ColorSpace";

            case TAG_ComponentsConfiguration: return "ComponentsConfiguration";
            case TAG_CompressedBitsPerPixel: return "CompressedBitsPerPixel";
            case TAG_PixelXDimension: return "PixelXDimension";
            case TAG_PixelYDimension: return "PixelYDimension";

            case TAG_MakerNote: return "MakerNote";
            case TAG_UserComment: return "UserComment";

            case TAG_RelatedSoundFile: return "RelatedSoundFile";

            case TAG_DateTimeOriginal: return "DateTimeOriginal";
            case TAG_DateTimeDigitized: return "DateTimeDigitized";
            case TAG_SubSecTime: return "SubSecTime";
            case TAG_SubSecTimeOriginal: return "SubSecTimeOriginal";
            case TAG_SubSecTimeDigitized: return "SubSecTimeDigitized";

            case TAG_ExposureTime: return "ExposureTime";
            case TAG_FNumber: return "FNumber";
            case TAG_ExposureProgram: return "ExposureProgram";
            case TAG_SpectralSensitivity: return "SpectralSensitivity";
            case TAG_PhotographicSensitivity: return "PhotographicSensitivity";
            case TAG_OECF: return "OECF";
            case TAG_SensitivityType: return "SensitivityType";
            case TAG_StandardOutputSensitivity: return "StandardOutputSensitivity";
            case TAG_RecommendedExposureIndex: return "RecommendedExposureIndex";
            case TAG_ISOSpeed: return "ISOSpeed";
            case TAG_ISOSpeedLatitudeyyy: return "ISOSpeedLatitudeyyy";
            case TAG_ISOSpeedLatitudezzz: return "ISOSpeedLatitudezzz";

            case TAG_ShutterSpeedValue: return "ShutterSpeedValue";
            case TAG_ApertureValue: return "ApertureValue";
            case TAG_BrightnessValue: return "BrightnessValue";
            case TAG_ExposureBiasValue: return "ExposureBiasValue";
            case TAG_MaxApertureValue: return "MaxApertureValue";
            case TAG_SubjectDistance: return "SubjectDistance";
            case TAG_MeteringMode: return "MeteringMode";
            case TAG_LightSource: return "LightSource";
            case TAG_Flash: return "Flash";
            case TAG_FocalLength: return "FocalLength";
            case TAG_SubjectArea: return "SubjectArea";
            case TAG_FlashEnergy: return "FlashEnergy";
            case TAG_SpatialFrequencyResponse: return "SpatialFrequencyResponse";
            case TAG_FocalPlaneXResolution: return "FocalPlaneXResolution";
            case TAG_FocalPlaneYResolution: return "FocalPlaneYResolution";
            case TAG_FocalPlaneResolutionUnit: return "FocalPlaneResolutionUnit";
            case TAG_SubjectLocation: return "SubjectLocation";
            case TAG_ExposureIndex: return "ExposureIndex";
            case TAG_SensingMethod: return "SensingMethod";
            case TAG_FileSource: return "FileSource";
            case TAG_SceneType: return "SceneType";
            case TAG_CFAPattern: return "CFAPattern";

            case TAG_CustomRendered: return "CustomRendered";
            case TAG_ExposureMode: return "ExposureMode";
            case TAG_WhiteBalance: return "WhiteBalance";
            case TAG_DigitalZoomRatio: return "DigitalZoomRatio";
            case TAG_FocalLengthIn35mmFormat: return "FocalLengthIn35mmFormat";
            case TAG_SceneCaptureType: return "SceneCaptureType";
            case TAG_GainControl: return "GainControl";
            case TAG_Contrast: return "Contrast";
            case TAG_Saturation: return "Saturation";
            case TAG_Sharpness: return "Sharpness";
            case TAG_DeviceSettingDescription: return "DeviceSettingDescription";
            case TAG_SubjectDistanceRange: return "SubjectDistanceRange";

            case TAG_ImageUniqueID: return "ImageUniqueID";
            case TAG_CameraOwnerName: return "CameraOwnerName";
            case TAG_BodySerialNumber: return "BodySerialNumber";
            case TAG_LensSpecification: return "LensSpecification";
            case TAG_LensMake: return "LensMake";
            case TAG_LensModel: return "LensModel";
            case TAG_LensSerialNumber: return "LensSerialNumber";
            case TAG_Gamma: return "Gamma";
            case TAG_PrintIM: return "PrintIM";
            case TAG_Padding: return "Padding";
            default: ;
        }
    } else if (ifdType == IFD_GPS) {
        switch (tagId) {
            case TAG_GPSVersionID: return "GPSVersionID";
            case TAG_GPSLatitudeRef: return "GPSLatitudeRef";
            case TAG_GPSLatitude: return "GPSLatitude";
            case TAG_GPSLongitudeRef: return "GPSLongitudeRef";
            case TAG_GPSLongitude: return "GPSLongitude";
            case TAG_GPSAltitudeRef: return "GPSAltitudeRef";
            case TAG_GPSAltitude: return "GPSAltitude";
            case TAG_GPSTimeStamp: return "GPSTimeStamp";
            case TAG_GPSSatellites: return "GPSSatellites";
            case TAG_GPSStatus: return "GPSStatus";
            case TAG_GPSMeasureMode: return "GPSMeasureMode";
            case TAG_GPSDOP: return "GPSDOP";
            case TAG_GPSSpeedRef: return "GPSSpeedRef";
            case TAG_GPSSpeed: return "GPSSpeed";
            case TAG_GPSTrackRef: return "GPSTrackRef";
            case TAG_GPSTrack: return "GPSTrack";
            case TAG_GPSImgDirectionRef: return "GPSImgDirectionRef";
            case TAG_GPSImgDirection: return "GPSImgDirection";
            case TAG_GPSMapDatum: return "GPSMapDatum";
            case TAG_GPSDestLatitudeRef: return "GPSDestLatitudeRef";
            case TAG_GPSDestLatitude: return "GPSDestLatitude";
            case TAG_GPSDestLongitudeRef: return "GPSDestLongitudeRef";
            case TAG_GPSDestLongitude: return "GPSDestLongitude";
            case TAG_GPSBearingRef: return "GPSBearingRef";
            case TAG_GPSBearing: return "GPSBearing";
            case TAG_GPSDestDistanceRef: return "GPSDestDistanceRef";
            case TAG_GPSDestDistance: return "GPSDestDistance";
            case TAG_GPSProcessingMethod: return "GPSProcessingMethod";
            case TAG_GPSAreaInformation: return "GPSAreaInformation";
            case TAG_GPSDateStamp: return "GPSDateStamp";
            case TAG_GPSDifferential: return "GPSDifferential";
            case TAG_GPSHPositioningError: return "GPSHPositioningError";
            default: ;
        }
    } else if (ifdType == IFD_IO) {
        switch (tagId) {
            case TAG_InteroperabilityIndex: return "InteroperabilityIndex";
            case TAG_InteroperabilityVersion: return "InteroperabilityVersion";
            case TAG_RelatedImageFileFormat: return "RelatedImageFileFormat";
            case TAG_RelatedImageWidth: return "RelatedImageWidth";
            case TAG_RelatedImageHeight: return "RelatedImageHeight";
            default: ;
        }
    } else if (ifdType == IFD_MPF) {
        switch (tagId) {
            case TAG_MPFVersion: return "MPFVersion";
            case TAG_NumberOfImage: return "NumberOfImage";
            case TAG_MPImageList: return "MPImageList";
            case TAG_ImageUIDList: return "ImageUIDList";
            case TAG_TotalFrames: return "TotalFrames";

            case TAG_MPIndividualNum: return "MPIndividualNum";
            case TAG_PanOrientation: return "PanOrientation";
            case TAG_PanOverlapH: return "PanOverlapH";
            case TAG_PanOverlapV: return "PanOverlapV";
            case TAG_BaseViewpointNum: return "BaseViewpointNum";
            case TAG_ConvergenceAngle: return "ConvergenceAngle";
            case TAG_BaselineLength: return "BaseLineLength";
            case TAG_VerticalDivergence: return "VerticalDivergence";
            case TAG_AxisDistanceX: return "AxisDistanceX";
            case TAG_AxisDistanceY: return "AxisDistanceY";
            case TAG_AxisDistanceZ: return "AxisDistanceZ";
            case TAG_YawAngle: return "YawAngle";
            case TAG_PitchAngle: return "PitchAngle";
            case TAG_RollAngle: return "RollAngle";
            default:;
        }
        return "(Unknown)";
    }
    return "(Unknown)";
}

// create the IFD table
static void *createIfdTable(EXIF_IFD_TYPE IfdType, uint16_t tagCount, unsigned int nextOfs)
{
    IfdTable *ifd = (IfdTable*)malloc(sizeof(IfdTable));
    if (!ifd) {
        return NULL;
    }
    memset(ifd, 0, sizeof(IfdTable));
    ifd->ifdType = IfdType;
    ifd->tagCount = tagCount;
    ifd->nextIfdOffset = nextOfs;
    return ifd;
}

// add the TagNode enrtry to the IFD table
static void *addTagNodeToIfd(void *pIfd,
                      uint16_t tagId,
                      uint16_t type,
                      unsigned int count,
                      unsigned int *numData,
                      uint8_t *byteData)
{
    int i;
    IfdTable *ifd = (IfdTable*)pIfd;
    TagNode *tag;
    if (!ifd) {
        return NULL;
    }
    tag = (TagNode*)malloc(sizeof(TagNode));
    memset(tag, 0, sizeof(TagNode));
    tag->tagId = tagId;
    tag->type = type;
    tag->count = count;

    if (count > 0) {
        if (numData != NULL) {
            int num = count;
            if (type == TYPE_RATIONAL ||
                type == TYPE_SRATIONAL) {
                num *= 2;
            }
            tag->numData = (unsigned int*)malloc(sizeof(int)*num);
            for (i = 0; i < num; i++) {
                tag->numData[i] = numData[i];
            }
        } else if (byteData != NULL) {
            tag->byteData = (uint8_t*)malloc(count);
            memcpy(tag->byteData, byteData, count);
        } else {
            tag->error = 1;
        }
    } else {
        tag->error = 1;
    }

    // first tag
    if (!ifd->tags) {
        ifd->tags = tag;
    } else {
        TagNode *tagWk = ifd->tags;
        while (tagWk->next) {
            tagWk = tagWk->next;
        }
        tagWk->next = tag;
        tag->prev = tagWk;
    }

    return tag;
}

// create a copy of TagNode
static TagNode *duplicateTagNode(TagNode *src)
{
    TagNode *dup;
    size_t len;
    if (!src || src->count <= 0) {
        return NULL;
    }
    dup = (TagNode*)malloc(sizeof(TagNode));
    memset(dup, 0, sizeof(TagNode));
    dup->tagId = src->tagId;
    dup->type = src->type;
    dup->count = src->count;
    dup->error = src->error;
    if (src->numData) {
        len = sizeof(int) * src->count;
        if (src->type == TYPE_RATIONAL ||
            src->type == TYPE_SRATIONAL) {
            len *= 2;
        }
        dup->numData = (unsigned int*)malloc(len);
        memcpy(dup->numData, src->numData, len);
    } else if (src->byteData) {
        len = sizeof(char) * src->count;
        dup->byteData = (uint8_t*)malloc(len);
        memcpy(dup->byteData, src->byteData, len);
    }
    return dup;
}

// free TagNode
static void freeTagNode(void *pTag)
{
    TagNode *tag = (TagNode*)pTag;
    if (!tag) {
        return;
    }
    if (tag->numData) {
        free(tag->numData);
    }
    if (tag->byteData) {
        free(tag->byteData);
    }
    free(tag);
}

// free entire IFD table
static void freeIfdTable(void *pIfd)
{
    IfdTable *ifd = (IfdTable*)pIfd;
    TagNode *tag;
    if (!ifd) {
        return;
    }
    tag = ifd->tags;
    if (ifd->p) {
        free(ifd->p);
    }
    free(ifd);

    if (tag) {
        while (tag->next) {
            tag = tag->next;
        }
        while (tag) {
            TagNode *tagWk = tag->prev;
            freeTagNode(tag);
            tag = tagWk;
        }
    }
    return;
}

// search the specified tag's node from the IFD table
static TagNode *getTagNodePtrFromIfd(IfdTable *ifd, uint16_t tagId)
{
    TagNode *tag;
    if (!ifd) {
        return NULL;
    }
    tag = ifd->tags;
    while (tag) {
        if (tag->tagId == tagId) {
            return tag;
        }
        tag = tag->next;
    }
    return NULL;
}

// remove the TagNode entry from the IFD table
static int removeTagOnIfd(void *pIfd, uint16_t tagId)
{
    int num = 0;
    IfdTable *ifd = (IfdTable*)pIfd;
    TagNode *tag;
    if (!ifd) {
        return 0;
    }
    for (;;) { // possibility of multiple entries
        tag = getTagNodePtrFromIfd(ifd, tagId);
        if (!tag) {
            break; // no more found
        }
        num++;
        if (tag->prev) {
            tag->prev->next = tag->next;
        } else {
            ifd->tags = tag->next;
        }
        if (tag->next) {
            tag->next->prev = tag->prev;
        }
        freeTagNode(tag);
        ifd->tagCount--;
    }
    return num;
}

// get the IFD table address of the specified type
static IfdTable *getIfdTableFromIfdTableArray(void **ifdTableArray, EXIF_IFD_TYPE ifdType)
{
    int i;
    if (!ifdTableArray) {
        return NULL;
    }
    for (i = 0; ifdTableArray[i] != NULL; i++) {
        IfdTable *ifd = ifdTableArray[i];
        if (ifd->ifdType == ifdType) {
            return ifd;
        }
    }
    return NULL;
}

// count IFD tables
static int countIfdTableOnIfdTableArray(void **ifdTableArray)
{
    int i, num = 0;
    for (i = 0; ifdTableArray[i] != NULL; i++) {
        num++;
    }
    return num;
}

// set single numeric value to the existing TagNode entry
static int setSingleNumDataToTag(TagNode *tag, unsigned int value)
{
    if (!tag) {
        return 0;
    }
    if (tag->type != TYPE_BYTE   &&
        tag->type != TYPE_SHORT  &&
        tag->type != TYPE_LONG   &&
        tag->type != TYPE_SBYTE  &&
        tag->type != TYPE_SSHORT &&
        tag->type != TYPE_SLONG) {
        return 0;
    }
    if (!tag->numData) {
        tag->numData = (unsigned int*)malloc(sizeof(int));
    }
    tag->count = 1;
    tag->numData[0] = value;
    tag->error = 0;
    return 1;
}

/**
 * write the Exif segment to the file
 *
 * parameters
 *  [in] fp: the output file pointer
 *  [in] ifdTableArray: address of the IFD tables array
 *
 * return
 *  0: OK
 *  EXIF_ERR_WRITE_FILE
 */
static int writeExifSegment(FILE *fp, void **ifdTableArray)
{
#define IFDMAX 5

    union _packed {
        unsigned int ui;
        uint16_t us[2];
        uint8_t uc[4];
    };

    IfdTable *ifds[IFDMAX], *ifd0th;
    TagNode *tag;
    IFD_TAG tagField;
    uint16_t num, us;
    unsigned int ui;
    int zero = 0;
    int i, x;
    unsigned int ofs;
    union _packed packed;
    APP_HEADER dupApp1Header = App1Header;

    ifds[0] = getIfdTableFromIfdTableArray(ifdTableArray, IFD_0TH);
    ifds[1] = getIfdTableFromIfdTableArray(ifdTableArray, IFD_EXIF);
    ifds[2] = getIfdTableFromIfdTableArray(ifdTableArray, IFD_IO);
    ifds[3] = getIfdTableFromIfdTableArray(ifdTableArray, IFD_GPS);
    ifds[4] = getIfdTableFromIfdTableArray(ifdTableArray, IFD_1ST);
    ifd0th = ifds[0];

    // return if 0th IFD is not exist
    if (!ifd0th) {
        return 0;
    }
    // get total length of the segment
    us = sizeof(APP_HEADER) - sizeof(short);
    for (x = 0; x < IFDMAX; x++) {
        if (ifds[x]) {
            us = us + ifds[x]->length;
        }
    }
    // segment length must be written in big-endian
    if (systemIsLittleEndian()) {
        us = swab16(us);
    }
    dupApp1Header.length = us;
    dupApp1Header.tiff.reserved = fix_short(dupApp1Header.tiff.reserved);
    dupApp1Header.tiff.Ifd0thOffset = fix_int(dupApp1Header.tiff.Ifd0thOffset);
    // write Exif segment Header
    if (fwrite(&dupApp1Header, 1, sizeof(APP_HEADER), fp) != sizeof(APP_HEADER)) {
        return EXIF_ERR_WRITE_FILE;
    }

    // base offset of the Exif segment
    ofs = sizeof(TIFF_HEADER);
    for (x = 0; x < IFDMAX; x++) {
        IfdTable *ifd = ifds[x];
        if (ifd == NULL) {
            continue;
        }
        // calculate the start offset to write the tag's data of this IFD
        ofs += sizeof(short) + // sizeof the tag number area
               sizeof(IFD_TAG) * ifd->tagCount + // sizeof the tag fields
               sizeof(int);    // sizeof the NextOffset area

        // write actual tag number of the current IFD
        num = 0;
        tag = ifd->tags;
        while (tag) {
            if (!tag->error) {
                num++;
            }
            tag = tag->next;
        }
        us = fix_short(num);
        if (fwrite(&us, 1, sizeof(short), fp) != sizeof(short)) {
            return EXIF_ERR_WRITE_FILE;
        }

        // write the each tag fields
        tag = ifd->tags;
        while (tag) {
            if (tag->error) {
                tag = tag->next; // ignore
                continue;
            }
            tagField.tag = fix_short(tag->tagId);
            tagField.type = fix_short(tag->type);
            tagField.count = fix_int(tag->count);
            packed.ui = 0;

            switch (tag->type) {
            case TYPE_ASCII:
            case TYPE_UNDEFINED:
                if (tag->count <= 4) {
                    for (i = 0; i < (int)tag->count; i++) {
                        packed.uc[i] = tag->byteData[i];
                    }
                } else {
                    packed.ui = fix_int(ofs);
                    ofs += tag->count;
                    if (tag->count % 2 != 0) {
                        ofs++;
                    }
                }
                break;
            case TYPE_BYTE:
            case TYPE_SBYTE:
                if (tag->count <= 4) {
                    for (i = 0; i < (int)tag->count; i++) {
                        packed.uc[i] = (uint8_t)tag->numData[i];
                    }
                } else {
                    packed.ui = fix_int(ofs);
                    ofs += tag->count;
                    if (tag->count % 2 != 0) {
                        ofs++;
                    }
                }
                break;
            case TYPE_SHORT:
            case TYPE_SSHORT:
                if (tag->count <= 2) {
                    for (i = 0; i < (int)tag->count; i++) {
                        packed.us[i] = fix_short((uint16_t)tag->numData[i]);
                    }
                } else {
                    packed.ui = fix_int(ofs);
                    ofs += tag->count * sizeof(short);
                }
                break;
            case TYPE_LONG:
            case TYPE_SLONG:
                if (tag->count <= 1) {
                    packed.ui = fix_int((unsigned int)tag->numData[0]);
                } else {
                    packed.ui = fix_int(ofs);
                    ofs += tag->count * sizeof(short);
                }
                break;
            case TYPE_RATIONAL:
            case TYPE_SRATIONAL:
                packed.ui = fix_int(ofs);
                ofs += tag->count * sizeof(int) * 2;
                break;
            }
            tagField.offset = packed.ui;
            if (fwrite(&tagField, 1, sizeof(tagField), fp) != sizeof(tagField)) {
                return EXIF_ERR_WRITE_FILE;
            }
            tag = tag->next;
        }
        ui = fix_int(ifd->nextIfdOffset);
        if (fwrite(&ui, 1, sizeof(int), fp) != sizeof(int)) {
            return EXIF_ERR_WRITE_FILE;
        }

        // write the tag values over 4 bytes
        tag = ifd->tags;
        while (tag) {
            if (tag->error) {
                tag = tag->next;
                continue;
            }
            switch (tag->type) {
            case TYPE_ASCII:
            case TYPE_UNDEFINED:
                if (tag->count > 4) {
                    if (fwrite(tag->byteData, 1, tag->count, fp) != tag->count) {
                        return EXIF_ERR_WRITE_FILE;
                    }
                    if (tag->count % 2 != 0) { // for even boundary
                        if (fwrite(&zero, 1, sizeof(char), fp) != sizeof(char)) {
                            return EXIF_ERR_WRITE_FILE;
                        }
                    }
                }
                break;
            case TYPE_BYTE:
            case TYPE_SBYTE:
                if (tag->count > 4) {
                    for (i = 0; i < (int)tag->count; i++) {
                        uint8_t n = (uint8_t)tag->numData[i];
                        if (fwrite(&n, 1, sizeof(char), fp) != sizeof(char)) {
                            return EXIF_ERR_WRITE_FILE;
                        }

                    }
                    if (tag->count % 2 != 0) {
                        if (fwrite(&zero, 1, sizeof(char), fp) != sizeof(char)) {
                            return EXIF_ERR_WRITE_FILE;
                        }
                    }
                }
                break;
            case TYPE_SHORT:
            case TYPE_SSHORT:
                if (tag->count > 2) {
                    for (i = 0; i < (int)tag->count; i++) {
                        uint16_t n = fix_short((uint16_t)tag->numData[i]);
                        if (fwrite(&n, 1, sizeof(short), fp) != sizeof(short)) {
                            return EXIF_ERR_WRITE_FILE;
                        }
                    }
                }
                break;
            case TYPE_LONG:
            case TYPE_SLONG:
                if (tag->count > 1) {
                    for (i = 0; i < (int)tag->count; i++) {
                        unsigned int n = fix_int((unsigned int)tag->numData[i]);
                        if (fwrite(&n, 1, sizeof(int), fp) != sizeof(int)) {
                            return EXIF_ERR_WRITE_FILE;
                        }
                    }
                }
                break;
            case TYPE_RATIONAL:
            case TYPE_SRATIONAL:
                for (i = 0; i < (int)tag->count*2; i++) {
                    unsigned int n = fix_int((unsigned int)tag->numData[i]);
                    if (fwrite(&n, 1, sizeof(int), fp) != sizeof(int)) {
                        return EXIF_ERR_WRITE_FILE;
                    }
                }
                break;
            }
            tag = tag->next;
        }
        // write the thumbnail data in the 1st IFD
        if (ifd->ifdType == IFD_1ST && ifd->p != NULL) {
            tag = getTagNodePtrFromIfd(ifd, TAG_JPEGInterchangeFormatLength);
            if (tag) {
                if (tag->numData[0] > 0) {
                    if (fwrite(ifd->p, 1, tag->numData[0], fp) != tag->numData[0]) {
                        return EXIF_ERR_WRITE_FILE;
                    }
                }
            }
        }
    }
    return 0;
}

// calculate the actual length of the IFD
static uint16_t calcIfdSize(void *pIfd)
{
    unsigned int size, num = 0;
    TagNode *tag;
    IfdTable *ifd = (IfdTable*)pIfd;
    if (!ifd) {
        return 0;
    }
    // count the actual tag number
    tag = ifd->tags;
    while (tag) {
        if (!tag->error) {
            num++;
        }
        tag = tag->next;
    }

    size = sizeof(short) + // sizeof the tag number area
           sizeof(IFD_TAG) * num + // sizeof tag fields
           sizeof(int); // sizeof the NextOffset area

    // add thumbnail data length
    if (ifd->ifdType == IFD_1ST) {
        if (ifd->p) {
            tag = getTagNodePtrFromIfd(ifd, TAG_JPEGInterchangeFormatLength);
            if (tag) {
                size += tag->numData[0];
            }
        }
    }
    tag = ifd->tags;
    while (tag) {
        if (tag->error) {
            // ignore
            tag = tag->next;
            continue;
        }
        switch (tag->type) {
        case TYPE_ASCII:
        case TYPE_UNDEFINED:
        case TYPE_BYTE:
        case TYPE_SBYTE:
            if (tag->count > 4) {
                size += tag->count;
                if (tag->count % 2 != 0) {
                    // padding for even byte boundary
                    size += 1;
                }
            }
            break;
        case TYPE_SHORT:
        case TYPE_SSHORT:
            if (tag->count > 2) {
                size += tag->count * sizeof(short);
            }
            break;
        case TYPE_LONG:
        case TYPE_SLONG:
            if (tag->count > 1) {
                size += tag->count * sizeof(int);
            }
            break;
        case TYPE_RATIONAL:
        case TYPE_SRATIONAL:
            if (tag->count > 0) {
                size += tag->count * sizeof(int) * 2;
            }
            break;
        }
        tag = tag->next;
    }
    return (uint16_t)size;
}

/**
 * refresh the length and offset variables in the IFD tables
 *
 * parameters
 *  [in/out] ifdTableArray: address of the IFD tables array
 *
 * return
 *  0: OK
 *  EXIF_ERR_INVALID_POINTER
 *  ERROR_UNKNOWN
 */
static int fixLengthAndOffsetInIfdTables(void **ifdTableArray)
{
    int i;
    TagNode *tag, *tagwk;
    uint16_t num;
    uint16_t ofsBase = sizeof(TIFF_HEADER);
    unsigned int len, dummy = 0, again = 0;
    IfdTable *ifd0th, *ifdExif, *ifdIo, *ifdGps, *ifd1st;
    if (!ifdTableArray) {
        return EXIF_ERR_INVALID_POINTER;
    }

AGAIN:
    // calculate the length of the each IFD tables.
    for (i = 0; ifdTableArray[i] != NULL; i++) {
        IfdTable *ifd = ifdTableArray[i];
        // count the actual tag number
        tag = ifd->tags;
        num = 0;
        while (tag) {
            // ignore and dispose the error tag
            if (tag->error) {
                tagwk = tag->next;
                if (tag->prev) {
                    tag->prev->next = tag->next;
                } else {
                    ifd->tags = tag->next;
                }
                if (tag->next) {
                    tag->next->prev = tag->prev;
                }
                freeTagNode(tag);
                tag = tagwk;
                continue;
            }
            num++;
            tag = tag->next;
        }
        ifd->tagCount = num;
        ifd->length = calcIfdSize(ifd);
        ifd->nextIfdOffset = 0;
    }
    ifd0th  = getIfdTableFromIfdTableArray(ifdTableArray, IFD_0TH);
    ifdExif = getIfdTableFromIfdTableArray(ifdTableArray, IFD_EXIF);
    ifdIo   = getIfdTableFromIfdTableArray(ifdTableArray, IFD_IO);
    ifdGps  = getIfdTableFromIfdTableArray(ifdTableArray, IFD_GPS);
    ifd1st  = getIfdTableFromIfdTableArray(ifdTableArray, IFD_1ST);

    if (!ifd0th) {
        return 0; // not error
    }
    ifd0th->offset = ofsBase;

    // set "NextOffset" variable in the 0th IFD if the 1st IFD is exist
    if (ifd1st) {
        ifd0th->nextIfdOffset =
                ofsBase +
                ifd0th->length +
                ((ifdExif)? ifdExif->length : 0) +
                ((ifdIo)? ifdIo->length : 0) +
                ((ifdGps)? ifdGps->length : 0);
        ifd1st->offset = (uint16_t)ifd0th->nextIfdOffset;

        // set the offset value of the thumbnail data
        if (ifd1st->p) {
            tag = getTagNodePtrFromIfd(ifd1st, TAG_JPEGInterchangeFormatLength);
            if (tag) {
                len = tag->numData[0]; // thumbnail length;
                tag = getTagNodePtrFromIfd(ifd1st, TAG_JPEGInterchangeFormat);
                if (tag) {
                    // set the offset value
                    setSingleNumDataToTag(tag, ifd1st->offset + ifd1st->length - len);
                } else {
                    // create the JPEGInterchangeFormat tag if not exist
                    if (!addTagNodeToIfd(ifd1st, TAG_JPEGInterchangeFormat,
                        TYPE_LONG, 1, &dummy, NULL)) {
                        return EXIF_ERR_UNKNOWN;
                    }
                    again = 1;
                }
            } else {
                tag = getTagNodePtrFromIfd(ifd1st, TAG_JPEGInterchangeFormat);
                if (tag) {
                    setSingleNumDataToTag(tag, 0);
                }
            }
        }
    } else {
        ifd0th->nextIfdOffset = 0; // 1st IFD is not exist
    }

    // set "ExifIFDPointer" tag value in the 0th IFD if the Exif IFD is exist
    if (ifdExif) {
        tag = getTagNodePtrFromIfd(ifd0th, TAG_ExifIFDPointer);
        if (tag) {
            setSingleNumDataToTag(tag, ofsBase + ifd0th->length);
            ifdExif->offset = (uint16_t)tag->numData[0];
        } else {
            // create the tag if not exist
            if (!addTagNodeToIfd(ifd0th, TAG_ExifIFDPointer, TYPE_LONG, 1,
                                    &dummy, NULL)) {
                return EXIF_ERR_UNKNOWN;
            }
            again = 1;
        }
        // set "InteroperabilityIFDPointer" tag value in the Exif IFD
        // if the Interoperability IFD is exist
        if (ifdIo) {
            tag = getTagNodePtrFromIfd(ifdExif, TAG_InteroperabilityIFDPointer);
            if (tag) {
                setSingleNumDataToTag(tag, ofsBase + ifd0th->length + ifdExif->length);
                ifdIo->offset = (uint16_t)tag->numData[0];
            } else {
                // create the tag if not exist
                if (!addTagNodeToIfd(ifdExif, TAG_InteroperabilityIFDPointer,
                                        TYPE_LONG, 1, &dummy, NULL)) {
                    return EXIF_ERR_UNKNOWN;
                }
                again = 1;
            }
        } else {
            tag = getTagNodePtrFromIfd(ifdExif, TAG_InteroperabilityIFDPointer);
            if (tag) {
                setSingleNumDataToTag(tag, 0);
            }
        }
    } else { // Exif
        tag = getTagNodePtrFromIfd(ifd0th, TAG_ExifIFDPointer);
        if (tag) {
            setSingleNumDataToTag(tag, 0);
        }
    }

    // set "GPSInfoIFDPointer" tag value in the 0th IFD if the GPS IFD is exist
    if (ifdGps) {
        tag = getTagNodePtrFromIfd(ifd0th, TAG_GPSInfoIFDPointer);
        if (tag) {
            setSingleNumDataToTag(tag, ofsBase +
                                        ifd0th->length +
                                        ((ifdExif)? ifdExif->length : 0) +
                                        ((ifdIo)? ifdIo->length : 0));
            ifdGps->offset = (uint16_t)tag->numData[0];
        } else {
            // create the tag if not exist
            if (!addTagNodeToIfd(ifd0th, TAG_GPSInfoIFDPointer, TYPE_LONG,
                                1, &dummy, NULL)) {
                return EXIF_ERR_UNKNOWN;
            }
            again = 1;
        }
    } else { // GPS IFD is not exist
        tag = getTagNodePtrFromIfd(ifd0th, TAG_GPSInfoIFDPointer);
        if (tag) {
            setSingleNumDataToTag(tag, 0);
        }
    }
    // repeat again if needed
    if (again) {
        again = 0;
        goto AGAIN;
    }
    return 0;
}

/**
 * Set the data of the IFD to the internal table
 *
 * parameters
 *  [in] fp: file pointer of opened file
 *  [in] startOffset : offset of target IFD
 *  [in] ifdType : type of the IFD
 *
 * return
 *   NULL: critical error occurred
 *  !NULL: the address of the IFD table
 */
static void *parseIFD(FILE *fp,
                      unsigned int baseOffset,
                      unsigned int startOffset,
                      EXIF_IFD_TYPE ifdType)
{
    void *ifd;
    uint8_t buf[8192];
    uint16_t tagCount, us;
    unsigned int nextOffset = 0;
    unsigned int *array, val, allocSize;
    int size, cnt, i;
    size_t len;
    int pos;

    // get the count of the tags
    if (seekToRelativeOffset(fp, baseOffset, startOffset) != 0 ||
        fread(&tagCount, 1, sizeof(short), fp) < sizeof(short)) {
        return NULL;
    }
    tagCount = fix_short(tagCount);
    pos = ftell(fp);

    // in case of the 0th IFD, check the offset of the 1st IFD
    if (ifdType == IFD_0TH || ifdType == IFD_MPF) {
        // next IFD's offset is at the tail of the segment
        if (seekToRelativeOffset(fp, baseOffset,
                sizeof(TIFF_HEADER) + sizeof(short) + sizeof(IFD_TAG) * tagCount) != 0 ||
            fread(&nextOffset, 1, sizeof(int), fp) < sizeof(int)) {
            return NULL;
        }
        nextOffset = fix_int(nextOffset);
        fseek(fp, pos, SEEK_SET);
    }
    // create new IFD table
    ifd = createIfdTable(ifdType, tagCount, nextOffset);

    // parse all tags
    for (cnt = 0; cnt < tagCount; cnt++) {
        IFD_TAG tag;
        uint8_t data[4];
        if (fseek(fp, pos, SEEK_SET) != 0 ||
            fread(&tag, 1, sizeof(tag), fp) < sizeof(tag)) {
            goto ERR;
        }
        memcpy(data, &tag.offset, 4); // keep raw data temporary
        tag.tag = fix_short(tag.tag);
        tag.type = fix_short(tag.type);
        tag.count = fix_int(tag.count);
        tag.offset = fix_int(tag.offset);
        pos = ftell(fp);

        //printf("tag=0x%04X type=%u count=%u offset=%u name=[%s]\n",
        //  tag.tag, tag.type, tag.count, tag.offset, getTagName(ifdType, tag.tag));

        if (tag.type == TYPE_ASCII ||     // ascii = the null-terminated string
            tag.type == TYPE_UNDEFINED) { // undefined = the chunk data bytes
            if (tag.count <= 4)  {
                // 4 bytes or less data is placed in the 'offset' area directly
                addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, data);
            } else {
                // 5 bytes or more data is placed in the value area of the IFD
                uint8_t *p = buf;
                if (tag.count > sizeof(buf)) {
                    // allocate new buffer if needed
                    if (tag.count >= App1Header.length) { // illegal
                        p = NULL;
                    } else {
                        p = (uint8_t*)malloc(tag.count);
                    }
                    if (!p) {
                        // treat as an error
                        addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, NULL);
                        continue;
                    }
                    memset(p, 0, tag.count);
                }
                if (seekToRelativeOffset(fp, baseOffset, tag.offset) != 0 ||
                    fread(p, 1, tag.count, fp) < tag.count) {
                    if (p != &buf[0]) {
                        free(p);
                    }
                    addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, NULL);
                    continue;
                }
                addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, p);
                if (p != &buf[0]) {
                    free(p);
                }
            }
        }
        else if (tag.type == TYPE_RATIONAL || tag.type == TYPE_SRATIONAL) {
            unsigned int realCount = tag.count * 2; // need double the space
            size_t len = realCount * sizeof(int);
            if (len >= App1Header.length) { // illegal
                array = NULL;
            } else {
                array = (unsigned int*)malloc(len);
                if (array) {
                    if (seekToRelativeOffset(fp, baseOffset, tag.offset) != 0 ||
                        fread(array, 1, len , fp) < len) {
                        free(array);
                        array = NULL;
                    } else {
                        for (i = 0; i < (int)realCount; i++) {
                            array[i] = fix_int(array[i]);
                        }
                    }
                }
            }
            addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, array, NULL);
            if (array) {
                free(array);
            }
        }
        else if (tag.type == TYPE_BYTE   ||
                 tag.type == TYPE_SHORT  ||
                 tag.type == TYPE_LONG   ||
                 tag.type == TYPE_SBYTE  ||
                 tag.type == TYPE_SSHORT ||
                 tag.type == TYPE_SLONG ) {

            // the single value is always stored in tag.offset area directly
            // # the data is Left-justified if less than 4 bytes
            if (tag.count <= 1) {
                val = tag.offset;
                if (tag.type == TYPE_BYTE || tag.type == TYPE_SBYTE) {
                    uint8_t uc = data[0];
                    val = uc;
                } else if (tag.type == TYPE_SHORT || tag.type == TYPE_SSHORT) {
                    memcpy(&us, data, sizeof(short));
                    us = fix_short(us);
                    val = us;
                }
                addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, &val, NULL);
             }
             // multiple value
             else {
                size = sizeof(int);
                if (tag.type == TYPE_BYTE || tag.type == TYPE_SBYTE) {
                    size = sizeof(char);
                } else if (tag.type == TYPE_SHORT || tag.type == TYPE_SSHORT) {
                    size = sizeof(short);
                }
                // for the sake of simplicity, using the 4bytes area for
                // each numeric data type
                allocSize = sizeof(int) * tag.count;
                if (allocSize >= App1Header.length) { // illegal
                    array = NULL;
                } else {
                    array = (unsigned int*)malloc(allocSize);
                }
                if (!array) {
                    addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, NULL);
                    continue;
                }
                len = size * tag.count;
                // if the total length of the value is less than or equal to 4bytes,
                // they have been stored in the tag.offset area
                if (len <= 4) {
                    if (size == 1) { // byte
                        for (i = 0; i < (int)tag.count; i++) {
                            array[i] = (unsigned int)data[i];
                        }
                    } else if (size == 2) { // short
                        for (i = 0; i < 2; i++) {
                            memcpy(&us, &data[i*2], sizeof(short));
                            us = fix_short(us);
                            array[i] = (unsigned int)us;
                        }
                    }
                } else {
                    if (seekToRelativeOffset(fp, baseOffset, tag.offset) != 0 ||
                        fread(buf, 1, len , fp) < len) {
                        addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, NULL, NULL);
                        continue;
                    }
                    for (i = 0; i < (int)tag.count; i++) {
                        memcpy(&val, &buf[i*size], size);
                        if (size == sizeof(int)) {
                            val = fix_int(val);
                        } else if (size == sizeof(short)) {
                            val = fix_short((uint16_t)val);
                        }
                        array[i] = (unsigned int)val;
                    }
                }
                addTagNodeToIfd(ifd, tag.tag, tag.type, tag.count, array, NULL);
                free(array);
             }
         }
    }
    if (ifdType == IFD_1ST) {
        // get thumbnail data
        unsigned int thumbnail_ofs = 0, thumbnail_len;
        IfdTable *ifdTable = (IfdTable*)ifd;
        TagNode *tag  = getTagNodePtrFromIfd(ifd, TAG_JPEGInterchangeFormat);
        if (tag) {
            thumbnail_ofs = tag->numData[0];
        }
        if (thumbnail_ofs > 0) {
            tag = getTagNodePtrFromIfd(ifd, TAG_JPEGInterchangeFormatLength);
            if (tag) {
                thumbnail_len = tag->numData[0];
                if (thumbnail_len > 0) {
                    ifdTable->p = (uint8_t*)malloc(thumbnail_len);
                    if (ifdTable->p) {
                        if (seekToRelativeOffset(fp, baseOffset, thumbnail_ofs) == 0) {
                            if (fread(ifdTable->p, 1, thumbnail_len, fp)
                                                        != thumbnail_len) {
                                free(ifdTable->p);
                                ifdTable->p = NULL;
                            } else {
                                // for test
                                //FILE *fpw = fopen("thumbnail.jpg", "wb");
                                //fwrite(ifdTable->p, 1, thumbnail_len, fpw);
                                //fclose(fpw);
                            }
                        } else {
                            free(ifdTable->p);
                            ifdTable->p = NULL;
                        }
                    }
                }
            }
        }
    }
    return ifd;
ERR:
    if (ifd) {
        freeIfdTable(ifd);
    }
    return NULL;
}


void setDefaultAppNSegmentHeader(APP_HEADER* appHeader, const char* strId, uint16_t marker)
{
    memset(&App1Header, 0, sizeof(APP_HEADER));
    appHeader->marker = systemIsLittleEndian() ? swab16(marker) : marker;
    appHeader->length = 0;
    strncpy(appHeader->id, strId, sizeof(appHeader->id));
    appHeader->tiff.byteOrder = 0x4949; // means little-endian
    appHeader->tiff.reserved = 0x002A;
    appHeader->tiff.Ifd0thOffset = 0x00000008;
}

void setDefaultMPFSegmentHeader(MPF_HEADER* appHeader, const char* strId, uint16_t marker)
{
    memset(&App1Header, 0, sizeof(APP_HEADER));
    appHeader->marker = systemIsLittleEndian() ? swab16(marker) : marker;
    appHeader->length = 0;
    strncpy(appHeader->id, strId, sizeof(appHeader->id));
    appHeader->tiff.byteOrder = 0x4949; // means little-endian
    appHeader->tiff.reserved = 0x002A;
    appHeader->tiff.Ifd0thOffset = 0x00000008;
}

/**
 * Load the APP1 segment header
 *
 * return
 *  1: success
 *  0: error
 */
static int readAppNSegmentHeader(FILE *fp, APP_HEADER* appHeader, size_t startOffset)
{
    // read the APP1 header
    if (fseek(fp, startOffset, SEEK_SET) != 0 ||
        fread(appHeader, 1, sizeof(APP_HEADER), fp) <
                                            sizeof(APP_HEADER)) {
        return 0;
    }
    if (systemIsLittleEndian()) {
        // the segment length value is always in big-endian order
        appHeader->length = swab16(appHeader->length);
    }
    // byte-order identifier
    if (appHeader->tiff.byteOrder != 0x4D4D && // big-endian
        appHeader->tiff.byteOrder != 0x4949) { // little-endian
        return 0;
    }
    // TIFF version number (always 0x002A)
    appHeader->tiff.reserved = fix_short(appHeader->tiff.reserved);
    if (appHeader->tiff.reserved != 0x002A) {
        return 0;
    }
    // offset of the 0TH IFD
    appHeader->tiff.Ifd0thOffset = fix_int(appHeader->tiff.Ifd0thOffset);
    return 1;
}

/**
* Load the MPF segment header
*
* return
*  1: success
*  0: error
*/
static int readMPFSegmentHeader(FILE *fp, MPF_HEADER* appHeader, size_t startOffset)
{
    // read the MPF header
    if (fseek(fp, startOffset, SEEK_SET) != 0 ||
        fread(appHeader, 1, sizeof(MPF_HEADER), fp) <
        sizeof(MPF_HEADER)) {
        return 0;
    }
    if (systemIsLittleEndian()) {
        // the segment length value is always in big-endian order
        appHeader->length = swab16(appHeader->length);
    }
    // byte-order identifier
    if (appHeader->tiff.byteOrder != 0x4D4D && // big-endian
        appHeader->tiff.byteOrder != 0x4949) { // little-endian
        return 0;
    }
    // TIFF version number (always 0x002A)
    appHeader->tiff.reserved = fix_short(appHeader->tiff.reserved);
    if (appHeader->tiff.reserved != 0x002A) {
        return 0;
    }
    // offset of the 0TH IFD
    appHeader->tiff.Ifd0thOffset = fix_int(appHeader->tiff.Ifd0thOffset);
    return 1;
}
/**
 * Get the offset of the Exif segment in the current opened JPEG file
 *
 * return
 *   n: the offset from the beginning of the file
 *   0: the Exif segment is not found
 *  -n: error
 */
#define EXIF_ID_STR     "Exif\0"
#define EXIF_ID_STR_LEN 5
#define FPXR_ID_STR     "FPXR\0"
#define FPXR_ID_STR_LEN 5
#define MPF_ID_STR        "MPF\0"
#define MPF_ID_STR_LEN    4

static int getAppNStartOffset(FILE *fp,
                              uint16_t appMarkerN,
                              const char *App1IDString,
                              size_t App1IDStringLength,
                              int *pDQTOffset)
{
    int pos;
    uint8_t buf[64];
    uint16_t len, marker;
    uint32_t appn_pos = 0;
    if (!fp) {
        return EXIF_ERR_READ_FILE;
    }
    rewind(fp);

    // check JPEG SOI Marker (0xFFD8)
    if (fread(&marker, 1, sizeof(short), fp) < sizeof(short)) {
        return EXIF_ERR_READ_FILE;
    }
    if (systemIsLittleEndian()) {
        marker = swab16(marker);
    }
    if (marker != 0xFFD8) {
        return EXIF_ERR_INVALID_JPEG;
    }
    // check for next 2 bytes
    if (fread(&marker, 1, sizeof(short), fp) < sizeof(short)) {
        return EXIF_ERR_READ_FILE;
    }
    if (systemIsLittleEndian()) {
        marker = swab16(marker);
    }
    // if DQT marker (0xFFDB) is appeared, the application segment
    // doesn't exist
    if (marker == 0xFFDB) {
        if (pDQTOffset != NULL) {
            *pDQTOffset = ftell(fp) - sizeof(short);
        }
        return 0; // not found the Exif segment
    }

    pos = ftell(fp);
    for (;;) {
        // unexpected value. is not a APP[0-14] marker
        if (!(marker >= 0xFFE0 && marker <= 0xFFEF)) {
            // found DQT
            if (marker == 0xFFDB && pDQTOffset != NULL) {
                *pDQTOffset = pos - sizeof(short);
                break;
            }
        }
        // read the length of the segment
        if (fread(&len, 1, sizeof(short), fp) < sizeof(short)) {
            return EXIF_ERR_READ_FILE;
        }
        if (systemIsLittleEndian()) {
            len = swab16(len);
        }
        // if is not a APPn segment, move to next segment
        if (marker != appMarkerN) {
            if (appn_pos != 0) {
                break;
            }
            if (fseek(fp, len - sizeof(short), SEEK_CUR) != 0) {
                return EXIF_ERR_INVALID_JPEG;
            }
        } else {
            // check if it is the Exif segment
            size_t bytesread = fread(buf, 1, App1IDStringLength + 4, fp);
            if (bytesread < App1IDStringLength) {
                return EXIF_ERR_READ_FILE;
            }
            if (memcmp(buf, App1IDString, App1IDStringLength) == 0) {
                // return the start offset of the Exif segment
                if (appn_pos == 0) {
                    appn_pos = pos - sizeof(short);
                }
            }
            if (Verbose) {
                unsigned char c1 = buf[0];
                unsigned char c2 = buf[1];
                unsigned char c3 = buf[2];
                unsigned char c4 = buf[3];
                if (c4 < ' ') {
                    c4 = '?';
                }
                printf("APP%u %c%c%c%c len=%u\n", appMarkerN - APP0_MARKER, c1, c2, c3, c4, len - 2);
            }
            // if is not a Exif segment, move to next segment
            if (fseek(fp, pos, SEEK_SET) != 0 ||
                fseek(fp, len, SEEK_CUR) != 0) {
                return EXIF_ERR_INVALID_JPEG;
            }
        }
        // read next marker
        if (fread(&marker, 1, sizeof(short), fp) < sizeof(short)) {
            return EXIF_ERR_READ_FILE;
        }
        if (systemIsLittleEndian()) {
            marker = swab16(marker);
        }
        pos = ftell(fp);
    }
    return appn_pos; // return Exif segment if found
}

/**
 * Initialize
 *
 * return
 *   1: OK
 *   0: the Exif segment is not found
 *  -n: error
 */
static int init(FILE *fp)
{
    int sts, dqtOffset = -1;;
    setDefaultAppNSegmentHeader(&App1Header, "Exif", 0xFFE1);
    setDefaultAppNSegmentHeader(&App2Header, "FPXR", 0xFFE2);
    setDefaultMPFSegmentHeader(&MPFHeader, "MPF", 0xFFE2);
    // get the offset of the Exif segment
    sts = getAppNStartOffset(fp, APP1_MARKER, EXIF_ID_STR, EXIF_ID_STR_LEN, &dqtOffset);
    if (sts < 0) { // error
        return sts;
    }
    JpegDQTOffset = dqtOffset;
    App1StartOffset = sts;
    if (sts == 0) {
        return sts;
    }

    App2StartOffset = getAppNStartOffset(fp, APP2_MARKER, FPXR_ID_STR, FPXR_ID_STR_LEN, NULL);

    MPFStartOffset = getAppNStartOffset(fp, APP2_MARKER, MPF_ID_STR, MPF_ID_STR_LEN, NULL);

    // Load the App1 segment header
    if (!readAppNSegmentHeader(fp, &App1Header, App1StartOffset)) {
        return EXIF_ERR_INVALID_APP1HEADER;
    }

    if (MPFStartOffset > 0) {
        if (!readMPFSegmentHeader(fp, &MPFHeader, MPFStartOffset)) {
            return EXIF_ERR_INVALID_APP1HEADER;
        }
    }
    return 1;
}

static void PRINTF(char **ms, const char *fmt, ...) {
    char buf[4096];
    char *p = NULL;
    int len, cnt;
    va_list args;
    va_start(args, fmt);
    cnt = vsnprintf(buf, sizeof(buf)-1, fmt, args);
    if (!ms) {
        printf("%s", buf);
        return;
    }
    else if (*ms) {
        len = (int)(strlen(*ms) + cnt + 1);
        p = (char*)malloc(len);
        strcpy(p, *ms);
        strcat(p, buf);
        free(*ms);
    } else {
        len = cnt + 1;
        p = (char*)malloc(len);
        strcpy(p, buf);
    }
    *ms = p;
    va_end(args);
}

#ifdef _MSC_VER
#ifdef __cplusplus
} /* extern "C" */
#endif
#endif
