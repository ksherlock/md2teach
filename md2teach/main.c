/*
 * main.c
 * md2teach
 *
 * Created by Jeremy Rand on 2021-04-13.
 * Copyright (c) 2021 Jeremy Rand. All rights reserved.
 *
 */


#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <gsos.h>
#include <resources.h>
#include <textedit.h>

#include "md4c.h"

#pragma memorymodel 1

// GS_TODO - How big does the stack need to be?  In looking over the code,
// I don't see massive stack frames due to large globals (other than the
// context which I made static).  But I do see lots of arguments and if
// the call stack gets deep enough, we could have a problem.
//
// Testing looks pretty good though with a trivial input.  The stack seems
// to reach just more than 512 bytes deep when the hook functions are called.
// I suspect things can get much worse with a complex document but this
// approach should let me measure the worst case stack with a complex
// document.
//
// Leaving the stack very big for now at 32K.
#pragma stacksize 32768

// Defines

#define NUM_HEADER_SIZES 6

// This is plain, emphasized, strong or strong+empasized
#define NUM_TEXT_FORMATS 4

#define NUM_HEADER_STYLES (NUM_HEADER_SIZES * NUM_TEXT_FORMATS)
#define NUM_CODE_STYLES 1
#define NUM_TEXT_STYLES NUM_TEXT_FORMATS
#define NUM_QUOTE_STYLES NUM_TEXT_FORMATS

#define TOTAL_STYLES (NUM_HEADER_STYLES + NUM_CODE_STYLES + NUM_TEXT_STYLES + NUM_QUOTE_STYLES)

// Typedefs

typedef struct tBlockListItem
{
    MD_BLOCKTYPE type;
    union {
        MD_BLOCK_UL_DETAIL ulDetail;
        MD_BLOCK_OL_DETAIL olDetail;
        MD_BLOCK_H_DETAIL hDetail;
        MD_BLOCK_CODE_DETAIL codeDetail;
    } u;
    int numTabs;
    
    struct tBlockListItem * next;
} tBlockListItem;

typedef struct tEntity
{
    const char * entityString;
    char entityChar;
    uint32_t unicodeChar;
} tEntity;

typedef struct tWindowPos
{
    int16_t height;
    int16_t width;
    int16_t top;
    int16_t left;
    int32_t version;
} tWindowPos;

// I wish I could use the structure definition from textedit.h but TERuler contains optional
// fields in the definition and Teach isn't expecting them it seems (array of theTabs).  So,
// I need my own struct which omits them.
typedef struct tRuler
{
    int16_t leftMargin;
    int16_t leftIndent;
    int16_t rightMargin;
    int16_t just;
    int16_t extraLS;
    int16_t flags;
    int32_t userData;
    int16_t tabType;
    int16_t tabTerminator;
} tRuler;

typedef struct tFormatHeader
{
    int16_t version;
    int32_t rulerSize;
    tRuler ruler;
    int32_t styleListLength;
    TEStyle styleList[TOTAL_STYLES];
    LongWord numberOfStyles;
} tFormatHeader;

typedef struct tFormat
{
    tFormatHeader header;
    StyleItem styleItems[1];
} tFormat;

//typedef struct tStyle

// Forward declarations

static int enterBlockHook(MD_BLOCKTYPE type, void * detail, void * userdata);
static int leaveBlockHook(MD_BLOCKTYPE type, void * detail, void * userdata);
static int enterSpanHook(MD_SPANTYPE type, void * detail, void * userdata);
static int leaveSpanHook(MD_SPANTYPE type, void * detail, void * userdata);
static int textHook(MD_TEXTTYPE type, const MD_CHAR * text, MD_SIZE size, void * userdata);
static void debugLogHook(const char * message, void * userdata);


// Globals

MD_PARSER parser = {
    0, // abi_version
    MD_FLAG_NOHTMLBLOCKS | MD_FLAG_NOHTMLSPANS, // flags
    enterBlockHook,
    leaveBlockHook,
    enterSpanHook,
    leaveSpanHook,
    textHook,
    debugLogHook,
    NULL // syntax
};

void * lowestStackSeen;
char * commandName;
int debugIndentLevel = 0;
int debugEnabled = 0;
int isFirstNonDocumentBlock = 1;

tBlockListItem * blockList = NULL;

IORecGS writeRec;
char writeBuffer[4096];
int writeBufferOffset = 0;

tWindowPos windowPos = {
    0xad,   // height
    0x27c,  // width
    0x1a,   // top
    0x02,   // left
    0x0     // version
};

// For the 6 header sizes, we are going with:
//      1 -> Helvetica 36
//      2 -> Helvetica 30
//      3 -> Helvetica 27
//      4 -> Helvetica 24
//      5 -> Helvetica 20
//      6 -> Helvetica 18
uint8_t headerFontSizes[NUM_HEADER_SIZES] = {
    36,
    30,
    27,
    24,
    20,
    18
};

tFormat * formatPtr = NULL;

tEntity entities[] = {
    { "&Tab;", 0x9, 0x9 },
    { "&NewLine;", 0x13, 0x10 },
    { "&excl;", 0x21, 0x21 },
    { "&quot;", 0x22, 0x22 },
    { "&QUOT;", 0x22, 0x22 },
    { "&num;", 0x23, 0x23 },
    { "&dollar;", 0x24, 0x24 },
    { "&percnt;", 0x25, 0x25 },
    { "&amp;", 0x26, 0x26 },
    { "&apos;", 0x27, 0x27 },
    { "&lpar;", 0x28, 0x28 },
    { "&rpar;", 0x29, 0x29 },
    { "&ast;", 0x2a, 0x2a },
    { "&midast;", 0x2a, 0x2a },
    { "&plus;", 0x2b, 0x2b },
    { "&comma;", 0x2c, 0x2c },
    { "&period;", 0x2e, 0x2e },
    { "&sol;", 0x2f, 0x2f },
    { "&colon;", 0x3a, 0x3a },
    { "&semi;", 0x3b, 0x3b },
    { "&lt;", 0x3c, 0x3c },
    { "&LT;", 0x3c, 0x3c },
    { "&equals;", 0x3d, 0x3d },
    { "&gt;", 0x3e, 0x3e },
    { "&GT;", 0x3e, 0x3e },
    { "&quest;", 0x3f, 0x3f },
    { "&commat;", 0x40, 0x40 },
    { "&lsqb;", 0x5b, 0x5b },
    { "&lbrack;", 0x5b, 0x5b },
    { "&bsol;", 0x5c, 0x5c },
    { "&rsqb;", 0x5d, 0x5d },
    { "&rbrack;", 0x5d, 0x5d },
    { "&Hat;", 0x5e, 0x5e },
    { "&lowbar;", 0x5f, 0x5f },
    { "&grave;", 0x60, 0x60 },
    { "&DiacriticalGrave;", 0x60, 0x60 },
    { "&lcub;", 0x7b, 0x7b },
    { "&lbrace;", 0x7b, 0x7b },
    { "&verbar;", 0x7c, 0x7c },
    { "&vert;", 0x7c, 0x7c },
    { "&VerticalLine;", 0x7c, 0x7c },
    { "&rcub;", 0x7d, 0x7d },
    { "&rbrace;", 0x7d, 0x7d },
    { "&nbsp;", 0xca, 0xa0 },
    { "&NonBreakingSpace;", 0xca, 0xa0 },
    { "&iexcl;", 0xc1, 0xa1 },
    { "&cent;", 0xa2, 0xa2 },
    { "&pound;", 0xa3, 0xa3 },
    { "&curren;", 0xdb, 0xa4 },
    { "&yen;", 0xb4, 0xa5 },
    { "&sect;", 0xa4, 0xa7 },
    { "&Dot;", 0xac, 0xa8 },
    { "&die;", 0xac, 0xa8 },
    { "&DoubleDot;", 0xac, 0xa8 },
    { "&uml;", 0xac, 0xa8 },
    { "&copy;", 0xa9, 0xa9 },
    { "&COPY;", 0xa9, 0xa9 },
    { "&ordf;", 0xbb, 0xaa },
    { "&laquo;", 0xc7, 0xab },
    { "&not;", 0xc2, 0xac },
    { "&reg;", 0xa8, 0xae },
    { "&circleR;", 0xa8, 0xae },
    { "&REG;", 0xa8, 0xae },
    { "&macr;", 0xf8, 0xaf },
    { "&OverBar;", 0xf8, 0xaf },
    { "&strns;", 0xf8, 0xaf },
    { "&deg;", 0xa1, 0xb0 },
    { "&plusmn;", 0xb1, 0xb1 },
    { "&pm;", 0xb1, 0xb1 },
    { "&PlusMinus;", 0xb1, 0xb1 },
    { "&acute;", 0xab, 0xb4 },
    { "&DiacriticalAcute;", 0xab, 0xb4 },
    { "&micro;", 0xb5, 0xb5 },
    { "&para;", 0xa6, 0xb6 },
    { "&middot;", 0xe1, 0xb7 },
    { "&centerdot;", 0xe1, 0xb7 },
    { "&CenterDot;", 0xe1, 0xb7 },
    { "&cedil;", 0xfc, 0xb8 },
    { "&Cedilla;", 0xfc, 0xb8 },
    { "&ordm;", 0xbc, 0xba },
    { "&raquo;", 0xc8, 0xbb },
    { "&iquest;", 0xc0, 0xbf },
    { "&Agrave;", 0xcb, 0xc0 },
    { "&Aacute;", 0xe7, 0xc1 },
    { "&Acirc;", 0xe5, 0xc2 },
    { "&Atilde;", 0xcc, 0xc3 },
    { "&Auml;", 0x80, 0xc4 },
    { "&Aring;", 0x81, 0xc5 },
    { "&AElig;", 0xae, 0xc6 },
    { "&Ccedil;", 0x82, 0xc7 },
    { "&Egrave;", 0xe9, 0xc8 },
    { "&Eacute;", 0x83, 0xc9 },
    { "&Ecirc;", 0xe6, 0xca },
    { "&Euml;", 0xe8, 0xcb },
    { "&Igrave;", 0xed, 0xcc },
    { "&Iacute;", 0xea, 0xcd },
    { "&Icirc;", 0xeb, 0xce },
    { "&Iuml;", 0xec, 0xcf },
    { "&Ntilde;", 0x84, 0xd1 },
    { "&Ograve;", 0xf1, 0xd2 },
    { "&Oacute;", 0xee, 0xd3 },
    { "&Ocirc;", 0xef, 0xd4 },
    { "&Otilde;", 0xcd, 0xd5 },
    { "&Ouml;", 0x85, 0xd6 },
    { "&Oslash;", 0xaf, 0xd8 },
    { "&Ugrave;", 0xf4, 0xd9 },
    { "&Uacute;", 0xf2, 0xda },
    { "&Ucirc;", 0xf3, 0xdb },
    { "&Uuml;", 0x86, 0xdc },
    { "&szlig;", 0xa7, 0xdf },
    { "&agrave;", 0x88, 0xe0 },
    { "&aacute;", 0x87, 0xe1 },
    { "&acirc;", 0x89, 0xe2 },
    { "&atilde;", 0x8b, 0xe3 },
    { "&auml;", 0x8a, 0xe4 },
    { "&aring;", 0x8c, 0xe5 },
    { "&aelig;", 0xbe, 0xe6 },
    { "&ccedil;", 0x8d, 0xe7 },
    { "&egrave;", 0x8f, 0xe8 },
    { "&eacute;", 0x8e, 0xe9 },
    { "&ecirc;", 0x90, 0xea },
    { "&euml;", 0x91, 0xeb },
    { "&igrave;", 0x93, 0xec },
    { "&iacute;", 0x92, 0xed },
    { "&icirc;", 0x94, 0xee },
    { "&iuml;", 0x95, 0xef },
    { "&ntilde;", 0x96, 0xf1 },
    { "&ograve;", 0x98, 0xf2 },
    { "&oacute;", 0x97, 0xf3 },
    { "&ocirc;", 0x99, 0xf4 },
    { "&otilde;", 0x9b, 0xf5 },
    { "&ouml;", 0x9a, 0xf6 },
    { "&divide;", 0xd6, 0xf7 },
    { "&div;", 0xd6, 0xf7 },
    { "&oslash;", 0xbf, 0xf8 },
    { "&ugrave;", 0x9d, 0xf9 },
    { "&uacute;", 0x9c, 0xfa },
    { "&ucirc;", 0x9e, 0xfb },
    { "&uuml;", 0x9f, 0xfc },
    { "&yuml;", 0xd8, 0xff },
    { "&dagger;", 0xa0, 0x2020 },
    { "&bull;", 0xa5, 0x2022 },
    { "&bullet;", 0xa5, 0x2022 },
    { "&trade;", 0xaa, 0x2122 },
    { "&TRADE;", 0xaa, 0x2122 },
    { "&ne;", 0xad, 0x2260 },
    { "&NotEqual;", 0xad, 0x2260 },
    { "&infin;", 0xb0, 0x221e },
    { "&le;", 0xb2, 0x2264 },
    { "&leq;", 0xb2, 0x2264 },
    { "&LessEqual;", 0xb2, 0x2264 },
    { "&ge;", 0xb3, 0x2265 },
    { "&geq;", 0xb3, 0x2265 },
    { "&GreaterEqual;", 0xb3, 0x2265 },
    { "&part;", 0xb6, 0x2202 },
    { "&PartialD;", 0xb6, 0x2202 },
    { "&sum;", 0xb7, 0x2211 },
    { "&Sum;", 0xb7, 0x2211 },
    { "&prod;", 0xb8, 0x220f },
    { "&Product;", 0xb8, 0x220f },
    { "&pi;", 0xb9, 0x3c0 },
    { "&int;", 0xba, 0x222b },
    { "&Integral;", 0xba, 0x222b },
    { "&Omega;", 0xbd, 0x3a9 },
    { "&radic;", 0xc3, 0x221a },
    { "&Sqrt;", 0xc3, 0x221a },
    { "&fnof;", 0xc4, 0x192 },
    { "&asymp;", 0xc5, 0x2248 },
    { "&ap;", 0xc5, 0x2248 },
    { "&TildeTilde;", 0xc5, 0x2248 },
    { "&approx;", 0xc5, 0x2248 },
    { "&thkap;", 0xc5, 0x2248 },
    { "&thickapprox;", 0xc5, 0x2248 },
    { "&Delta;", 0xc6, 0x394 },
    { "&hellip;", 0xc9, 0x2026 },
    { "&mldr;", 0xc9, 0x2026 },
    { "&OElig;", 0xce, 0x152 },
    { "&oelig;", 0xcf, 0x153 },
    { "&ndash;", 0xd0, 0x2013 },
    { "&mdash;", 0xd1, 0x2014 },
    { "&ldquo;", 0xd2, 0x201c },
    { "&OpenCurlyDoubleQuote;", 0xd2, 0x201c },
    { "&rdquo;", 0xd3, 0x201d },
    { "&rdquor;", 0xd3, 0x201d },
    { "&CloseCurlyDoubleQuote;", 0xd3, 0x201d },
    { "&lsquo;", 0xd4, 0x2018 },
    { "&OpenCurlyQuote;", 0xd4, 0x2018 },
    { "&rsquo;", 0xd5, 0x2019 },
    { "&rsquor;", 0xd5, 0x2019 },
    { "&CloseCurlyQuote;", 0xd5, 0x2019 },
    { "&loz;", 0xd7, 0x25ca },
    { "&lozenge;", 0xd7, 0x25ca },
    { "&Yuml;", 0xd9, 0x178 },
    { "&frasl;", 0xda, 0x2044 },
    { "&lsaquo;", 0xdc, 0x2039 },
    { "&rsaquo;", 0xdd, 0x203a },
    { "&filig;", 0xde, 0xfb01 },
    { "&fllig;", 0xdf, 0xfb02 },
    { "&Dagger;", 0xe0, 0x2021 },
    { "&ddagger;", 0xe0, 0x2021 },
    { "&lsquor;", 0xe2, 0x201a },
    { "&sbquo;", 0xe2, 0x201a },
    { "&ldquor;", 0xe3, 0x201e },
    { "&bdquo;", 0xe3, 0x201e },
    { "&permil;", 0xe4, 0x2030 },
    { "", 0xf0, 0xf8ff },
    { "&imath;", 0xf5, 0x131 },
    { "&inodot;", 0xf5, 0x131 },
    { "&circ;", 0xf6, 0x2c6 },
    { "&tilde;", 0xf7, 0x2dc },
    { "&DiacriticalTilde;", 0xf7, 0x2dc },
    { "&breve;", 0xf9, 0x2d8 },
    { "&Breve;", 0xf9, 0x2d8 },
    { "&dot;", 0xfa, 0x2d9 },
    { "&DiacriticalDot;", 0xfa, 0x2d9 },
    { "&ring;", 0xfb, 0x2da },
    { "&dblac;", 0xfd, 0x2dd },
    { "&DiacriticalDoubleAcute;", 0xfd, 0x2dd },
    { "&ogon;", 0xfe, 0x2db },
    { "&caron;", 0xff, 0x2c7 },
    { "&Hacek;", 0xff, 0x2c7 },
    
    // GS_TODO - Test each of these entities.
};

// Implementation

void flushBuffer(void)
{
    writeRec.requestCount = writeBufferOffset;
    WriteGS(&writeRec);
    if (toolerror()) {
        fprintf(stderr, "%s: Error writing to output file\n", commandName);
        exit(1);
    }
    writeBufferOffset = 0;
}

void writeChar(MD_CHAR ch)
{
    if (writeBufferOffset == sizeof(writeBuffer))
        flushBuffer();
    
    if (ch == '\n')
        ch = '\r';
    writeBuffer[writeBufferOffset] = ch;
    writeBufferOffset++;
}

void writeString(MD_CHAR * str, MD_SIZE size)
{
    MD_SIZE i;
    
    for (i = 0; i < size; i++)
        writeChar(str[i]);
}

static int enterBlockHook(MD_BLOCKTYPE type, void * detail, void * userdata)
{
    tBlockListItem * newBlock = malloc(sizeof(tBlockListItem));
    
    if (newBlock == NULL) {
        fprintf(stderr, "%s: Out of memory", commandName);
        return 1;
    }
    
    newBlock->type = type;
    if (blockList == NULL)
        newBlock->numTabs = 0;
    else
        newBlock->numTabs = blockList->numTabs;
    newBlock->next = blockList;
    blockList = newBlock;
    
    if ((detail != NULL) &&
        (detail < lowestStackSeen))
        lowestStackSeen = detail;
    
    switch (type) {
        case MD_BLOCK_DOC:
            if (debugEnabled)
                fprintf(stderr, "%*sDOC {\n", debugIndentLevel, "");
            break;
            
        case MD_BLOCK_QUOTE:
            if (debugEnabled)
                fprintf(stderr, "%*sQUOTE {\n", debugIndentLevel, "");
            
            break;
            
        case MD_BLOCK_UL: {
            MD_BLOCK_UL_DETAIL * ulDetail = (MD_BLOCK_UL_DETAIL *)detail;
            if (debugEnabled)
                fprintf(stderr, "%*sUL (is_tight=%d, mark=%c) {\n", debugIndentLevel, "", ulDetail->is_tight, ulDetail->mark);
            
            memcpy(&(newBlock->u.ulDetail), ulDetail, sizeof(*ulDetail));
            newBlock->numTabs++;
            
            if (!isFirstNonDocumentBlock)
                writeChar('\r');
            break;
        }
            
        case MD_BLOCK_OL: {
            MD_BLOCK_OL_DETAIL * olDetail = (MD_BLOCK_OL_DETAIL *)detail;
            if (debugEnabled)
                fprintf(stderr, "%*sOL (start=%u, is_tight=%d, mark_delimiter=%c) {\n", debugIndentLevel, "", olDetail->start, olDetail->is_tight, olDetail->mark_delimiter);
            
            memcpy(&(newBlock->u.olDetail), olDetail, sizeof(*olDetail));
            newBlock->numTabs++;
            
            if (!isFirstNonDocumentBlock)
                writeChar('\r');
            break;
        }
            
        case MD_BLOCK_LI: {
            int i;
            tBlockListItem * enclosingBlock = newBlock->next;
            int isNumbered = 0;
            static char str[16];
            
            if (debugEnabled)
                fprintf(stderr, "%*sLI {\n", debugIndentLevel, "");
            
            if (enclosingBlock == NULL) {
                fprintf(stderr, "%s: Got a list item block without an enclosing block\n", commandName);
                return 1;
            }
            
            if (enclosingBlock->type == MD_BLOCK_OL) {
                isNumbered = 1;
                if ((!enclosingBlock->u.olDetail.is_tight) &&
                    (!isFirstNonDocumentBlock))
                    writeChar('\r');
            } else if (enclosingBlock->type == MD_BLOCK_UL) {
                if ((!enclosingBlock->u.ulDetail.is_tight) &&
                    (!isFirstNonDocumentBlock))
                    writeChar('\r');
            }
            
            for (i = 0; i < newBlock->numTabs; i++)
                writeChar('\t');
            
            if (isNumbered) {
                sprintf(str, "%u%c ", enclosingBlock->u.olDetail.start, enclosingBlock->u.olDetail.mark_delimiter);
                enclosingBlock->u.olDetail.start++;
            } else {
                sprintf(str, "%c ", 0xa5);    // 0xa5 is a bullet character
            }
            writeString(str, strlen(str));
            
            break;
        }
            
        case MD_BLOCK_HR: {
            int i;
            
            if (debugEnabled)
                fprintf(stderr, "%*sHR {\n", debugIndentLevel, "");
            
            if (!isFirstNonDocumentBlock)
                writeChar('\r');
            
            for (i = 0; i < 30; i++)
                writeChar('_');
            break;
        }
            
        case MD_BLOCK_H: {
            MD_BLOCK_H_DETAIL * hDetail = (MD_BLOCK_H_DETAIL *)detail;
            if (debugEnabled)
                fprintf(stderr, "%*sH (level=%u) {\n", debugIndentLevel, "", hDetail->level);
            
            memcpy(&(newBlock->u.hDetail), hDetail, sizeof(*hDetail));
            
            if (!isFirstNonDocumentBlock)
                writeChar('\r');
            break;
        }
            
        case MD_BLOCK_CODE: {
            MD_BLOCK_CODE_DETAIL * codeDetail = (MD_BLOCK_CODE_DETAIL *)detail;
            if (debugEnabled) {
                fprintf(stderr, "%*sCODE ", debugIndentLevel, "");
                if (codeDetail->fence_char != '\0') {
                    fprintf(stderr, "(fence_char=%c) ", codeDetail->fence_char);
                }
                fprintf(stderr, "{\n");
            }
            
            memcpy(&(newBlock->u.codeDetail), codeDetail, sizeof(*codeDetail));
            
            if (!isFirstNonDocumentBlock)
                writeChar('\r');
            break;
        }
            
        case MD_BLOCK_P:
            if (debugEnabled)
                fprintf(stderr, "%*sP {\n", debugIndentLevel, "");
            
            if (!isFirstNonDocumentBlock)
                writeChar('\r');
            break;
            
        default:
            fprintf(stderr, "%s: Invalid block type (%d)\n", commandName, (int)type);
            return 1;
            break;
    }
    
    
    if (type != MD_BLOCK_DOC)
        isFirstNonDocumentBlock = 0;
    
    debugIndentLevel+=2;
    return 0;
}


static int leaveBlockHook(MD_BLOCKTYPE type, void * detail, void * userdata)
{
    tBlockListItem * oldBlock = blockList;
    
    if (oldBlock == NULL) {
        fprintf(stderr, "%s: Block list is empty but leaving block of type %d\n", commandName, (int)type);
        return 1;
    }
    
    if (oldBlock->type != type) {
        fprintf(stderr, "%s: Expected to leave block of type %d but got type %d\n", commandName, (int)oldBlock->type, (int)type);
        return 1;
    }
    
    blockList = oldBlock->next;
    free(oldBlock);
    
    if ((detail != NULL) &&
        (detail < lowestStackSeen))
        lowestStackSeen = detail;
    
    switch (type) {
        case MD_BLOCK_DOC:
            break;
            
        case MD_BLOCK_QUOTE:
            break;
            
        case MD_BLOCK_UL:
            writeChar('\r');
            break;
            
        case MD_BLOCK_OL:
            writeChar('\r');
            break;
            
        case MD_BLOCK_LI:
            writeChar('\r');
            break;
            
        case MD_BLOCK_HR:
            writeChar('\r');
            break;
            
        case MD_BLOCK_H:
            writeChar('\r');
            break;
            
        case MD_BLOCK_CODE:
            writeChar('\r');
            break;
            
        case MD_BLOCK_P:
            writeChar('\r');
            break;
            
        default:
            fprintf(stderr, "%s: Invalid block type (%d)\n", commandName, (int)type);
            return 1;
            break;
    }
    
    debugIndentLevel-=2;
    if (debugEnabled)
        fprintf(stderr, "%*s}\n", debugIndentLevel, "");
    
    return 0;
}


static int enterSpanHook(MD_SPANTYPE type, void * detail, void * userdata)
{
    if ((detail != NULL) &&
        (detail < lowestStackSeen))
        lowestStackSeen = detail;
    
    switch (type) {
        case MD_SPAN_EM:
            if (debugEnabled)
                fprintf(stderr, "%*sEM {\n", debugIndentLevel, "");
            break;
            
        case MD_SPAN_STRONG:
            if (debugEnabled)
                fprintf(stderr, "%*sSTRONG {\n", debugIndentLevel, "");
            break;
            
        case MD_SPAN_A:
            if (debugEnabled)
                fprintf(stderr, "%*sA {\n", debugIndentLevel, "");
            break;
            
        case MD_SPAN_IMG:
            if (debugEnabled)
                fprintf(stderr, "%*sIMG {\n", debugIndentLevel, "");
            break;
            
        case MD_SPAN_CODE:
            if (debugEnabled)
                fprintf(stderr, "%*sCODE {\n", debugIndentLevel, "");
            break;
            
        default:
            fprintf(stderr, "%s: Invalid span type (%d)\n", commandName, (int)type);
            return 1;
            break;
    }
    
    debugIndentLevel+=2;
    return 0;
}


static int leaveSpanHook(MD_SPANTYPE type, void * detail, void * userdata)
{
    if ((detail != NULL) &&
        (detail < lowestStackSeen))
        lowestStackSeen = detail;
    
    switch (type) {
        case MD_SPAN_EM:
            break;
            
        case MD_SPAN_STRONG:
            break;
            
        case MD_SPAN_A:
            break;
            
        case MD_SPAN_IMG:
            break;
            
        case MD_SPAN_CODE:
            break;
            
        default:
            fprintf(stderr, "%s: Invalid span type (%d)\n", commandName, (int)type);
            return 1;
            break;
    }
    
    debugIndentLevel-=2;
    if (debugEnabled)
        fprintf(stderr, "%*s}\n", debugIndentLevel, "");
    
    return 0;
}

static void printEntity(const MD_CHAR * text, MD_SIZE size)
{
    int entityNum;
    uint32_t unicodeChar = 0;
    
    if (size < 4)
        return;
    
    if (text[0] != '&')
        return;
    
    if (text[size - 1] != ';')
        return;
    
    if (text[1] == '#') {
        char * end;
        unicodeChar = strtoul(text + 2, &end, 10);
        if (end != text + size - 1)
            unicodeChar = 0;
        if ((unicodeChar > 0) &&
            (unicodeChar < 128)) {
            writeChar(unicodeChar);
            return;
        }
    }
    
    if (text[1] == 'x') {
        char * end;
        unicodeChar = strtoul(text + 2, &end, 16);
        if (end != text + size - 1)
            unicodeChar = 0;
        if ((unicodeChar > 0) &&
            (unicodeChar < 128)) {
            writeChar(unicodeChar);
            return;
        }
    }
    
    for (entityNum = 0; entityNum < (sizeof(entities) / sizeof(entities[0])); entityNum++) {
        if ((unicodeChar == entities[entityNum].unicodeChar) ||
            (strncmp(entities[entityNum].entityString, text, size) == 0)) {
            writeChar(entities[entityNum].entityChar);
            return;
        }
    }
}

static int textHook(MD_TEXTTYPE type, const MD_CHAR * text, MD_SIZE size, void * userdata)
{
    switch (type) {
        case MD_TEXT_NORMAL:
            if (debugEnabled)
                fprintf(stderr, "%*sText: \"", debugIndentLevel, "");
            break;
        
        case MD_TEXT_NULLCHAR:
            fprintf(stderr, "%s: Null character encountered on input\n", commandName);
            return 1;
            
        case MD_TEXT_BR:
            if (debugEnabled)
                fprintf(stderr, "%*sBR\n", debugIndentLevel, "");
            putchar('\n');
            return 0;
            
        case MD_TEXT_SOFTBR:
            if (debugEnabled)
                fprintf(stderr, "%*sSOFT BR\n", debugIndentLevel, "");
            return 0;
            
        case MD_TEXT_ENTITY:
            if (debugEnabled) {
                fprintf(stderr, "%*sEntity: \"", debugIndentLevel, "");
                fwrite(text, sizeof(MD_CHAR), size, stderr);
            }
            
            printEntity(text, size);
            text = "";
            size = 0;
            break;
            
        case MD_TEXT_CODE:
            if (debugEnabled)
                fprintf(stderr, "%*sCode: \"", debugIndentLevel, "");
            break;
            
        default:
            fprintf(stderr, "%s: Invalid text type (%d)\n", commandName, (int)type);
            return 1;
            break;
    }
    
    if (debugEnabled) {
        fwrite(text, sizeof(MD_CHAR), size, stderr);
        fprintf(stderr, "\"\n");
    }
    
    if (size > 0)
        writeString(text, size);
    
    return 0;
}


static void debugLogHook(const char * message, void * userdata)
{
    if (debugEnabled)
        fprintf(stderr, "DEBUG: %s\n", message);
}


static void printUsage(void)
{
    fprintf(stderr, "USAGE: %s [ -d ] inputfile outputfile\n", commandName);
    exit(1);
}


static int parseArgs(int argc, char * argv[])
{
    commandName = argv[0];
    
    static int index;
    static int charOffset;
    static int optionLen;
    
    for (index = 1; index < argc; index++) {
        if (argv[index][0] != '-')
            break;
        
        optionLen = strlen(argv[index]);
        for (charOffset = 1; charOffset < optionLen; charOffset++) {
            switch (argv[index][charOffset]) {
                case 'd':
                    debugEnabled = 1;
                    break;
                
                default:
                    printUsage();
                    break;
            }
        }
    }
    
    if (index + 2 != argc) {
        printUsage();
    }
    
    return index;
}


int main(int argc, char * argv[])
{
    int result;
    
    static char * inputFileName;
    static FILE * inputFile;
    static long inputFileLen;
    static char * inputBuffer;

    static GSString255 outputFileName;
    static CreateRecGS createRec;
    static NameRecGS destroyRec;
    static OpenRecGS openRec;
    static RefNumRecGS closeRec;
    
    static int index;
    
    lowestStackSeen = &result;
    
    index = parseArgs(argc, argv);
    inputFileName = argv[index];
    
    outputFileName.length = strlen(argv[index + 1]);
    if (outputFileName.length >= sizeof(outputFileName.text)) {
        fprintf(stderr, "%s: Output file path too long, %s\n", commandName, outputFileName);
        exit(1);
    }
    strcpy(outputFileName.text, argv[index + 1]);
    
    destroyRec.pCount = 1;
    destroyRec.pathname = &outputFileName;
    DestroyGS(&destroyRec);
    
    createRec.pCount = 7;
    createRec.pathname = &outputFileName;
    createRec.access = destroyEnable | renameEnable | readWriteEnable;
    createRec.fileType = 0x50; // Type for Teach file
    createRec.auxType = 0x5445; // Aux type for Teach file
    createRec.storageType = extendedFile;
    CreateGS(&createRec);
    if (toolerror()) {
        fprintf(stderr, "%s: Unable to create output file %s\n", commandName, outputFileName.text);
        exit(1);
    }
    
    openRec.pCount = 3;
    openRec.refNum = 0;
    openRec.pathname = &outputFileName;
    openRec.requestAccess = writeEnable;
    OpenGS(&openRec);
    if (toolerror()) {
        fprintf(stderr, "%s: Unable to open output file %s\n", commandName, outputFileName.text);
        exit(1);
    }
    
    writeRec.pCount = 4;
    writeRec.refNum = openRec.refNum;
    writeRec.dataBuffer = writeBuffer;
    
    inputFile = fopen(inputFileName, "r");
    if (inputFile == NULL) {
        fprintf(stderr, "%s: Unable to open input file %s, %s\n", commandName, inputFileName, strerror(errno));
        exit(1);
    }
    
    if (fseek(inputFile, 0l, SEEK_END) != 0) {
        fprintf(stderr, "%s: Unable to seek to the end of file %s, %s\n", commandName, inputFileName, strerror(errno));
        fclose(inputFile);
        exit(1);
    }
    
    inputFileLen = ftell(inputFile);
    if (inputFileLen < 0) {
        fprintf(stderr, "%s: Unable to get size of file %s, %s\n", commandName, inputFileName, strerror(errno));
        fclose(inputFile);
        exit(1);
    }
    
    inputBuffer = malloc(inputFileLen);
    if (inputBuffer == NULL) {
        fprintf(stderr, "%s: Unable to allocate %ld bytes for input buffer\n", commandName, inputFileLen);
        fclose(inputFile);
        exit(1);
    }
    
    if (fseek(inputFile, 0l, SEEK_SET) != 0) {
        fprintf(stderr, "%s: Unable to seek to the beginning of file %s, %s\n", commandName, inputFileName, strerror(errno));
        free(inputBuffer);
        fclose(inputFile);
        exit(1);
    }
    
    if (fread(inputBuffer, 1, inputFileLen, inputFile) != inputFileLen) {
        fprintf(stderr, "%s: Unable to read all of file %s, %s\n", commandName, inputFileName, strerror(errno));
        free(inputBuffer);
        fclose(inputFile);
        exit(1);
    }
    
    result = md_parse(inputBuffer, inputFileLen, &parser, NULL);
    
    if (writeBufferOffset > 0)
        flushBuffer();
    closeRec.pCount = 1;
    closeRec.refNum = openRec.refNum;
    CloseGS(&closeRec);
    
    putchar('\n');
    
    fclose(inputFile);
    
    if (debugEnabled) {
        fprintf(stderr, "Parser result: %d\n", result);
        fprintf(stderr, "Most stack used: %lu\n", ((unsigned long)&result) - ((unsigned long)lowestStackSeen));
    }
    
    return 0;
}
