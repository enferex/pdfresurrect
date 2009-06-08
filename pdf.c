/******************************************************************************
 * pdf.c 
 *
 * pdfresurrect - PDF history extraction tool
 *
 * Copyright (C) 2008, 2009 Matt Davis (enferex) of 757Labs (www.757labs.com)
 *
 * pdf.c is part of pdfresurrect.
 * pdfresurrect is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pdfresurrect is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pdfresurrect.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "pdf.h"
#include "main.h"


/*
 * Forwards
 */

static int is_valid_xref(FILE *fp, pdf_t *pdf, xref_t *xref);
static void load_xref_entries(FILE *fp, xref_t *xref);
static void load_xref_from_plaintext(FILE *fp, xref_t *xref);
static void load_xref_from_stream(FILE *fp, xref_t *xref);
static pdf_creator_t *new_creator(int *n_elements);
static void load_creator(FILE *fp, pdf_t *);
static void load_creator_from_buf(xref_t *xref, const char *buf);
static void load_creator_from_xml(xref_t *xref, const char *buf);
static void load_creator_from_old_format(xref_t *xref, const char *buf);

static char *get_object_from_here(FILE *fp, size_t *size, int *is_stream);

static char *get_object(
    FILE         *fp,
    int           obj_id,
    const xref_t *xref,
    size_t       *size,
    int          *is_stream);

static void add_kid(int id, xref_t *xref);
static void load_kids(FILE *fp, int pages_id, xref_t *xref);

static const char *get_type(FILE *fp, int obj_id, const xref_t *xref);
static int get_page(int obj_id, const xref_t *xref);
static char *get_header(FILE *fp);


/*
 * Defined
 */

pdf_t *pdf_new(const char *name)
{
    const char *n;
    pdf_t      *pdf;
   
    pdf = calloc(1, sizeof(pdf_t));

    if (name)
    {
        /* Just get the file name (not path) */
        if ((n = strrchr(name, '/')))
          ++n;
        else
          n = name;

        pdf->name = malloc(strlen(n) + 1);
        strcpy(pdf->name, n);
    }
    else /* !name */
    {
        pdf->name = malloc(strlen("Unknown") + 1);
        strcpy(pdf->name, "Unknown");
    }

    return pdf;
}


void pdf_delete(pdf_t *pdf)
{
    int i;

    for (i=0; i<pdf->n_xrefs; i++)
    {
        free(pdf->xrefs[i].entries);
        free(pdf->xrefs[i].kids);
    }

    free(pdf->name);
    free(pdf->xrefs);
    free(pdf);
}


int pdf_is_pdf(FILE *fp)
{
    int   is_pdf;
    char *header;

    header = get_header(fp);

    if (header && strstr(header, "%PDF-"))
      is_pdf = 1;
    else 
      is_pdf = 0;

    free(header);
    return is_pdf;
}


void pdf_get_version(FILE *fp, pdf_t *pdf)
{
    char *header, *c;

    header = get_header(fp);

    /* Locate version string start and make sure we dont go past header */
    if ((c = strstr(header, "%PDF-")) && 
        (c + strlen("%PDF-M.m") + 2))
    {
        pdf->pdf_major_version = atoi(c + strlen("%PDF-"));
        pdf->pdf_minor_version = atoi(c + strlen("%PDF-M."));
    }

    free(header);
}


int pdf_load_xrefs(FILE *fp, pdf_t *pdf)
{
    long pos, pos_count;
    int  i, ver;
    char x, *c, buf[256];
    
    c = NULL;

    /* Count number of xrefs */
    pdf->n_xrefs = 0;
    fseek(fp, 0, SEEK_SET);
    while (fgets(buf, 256, fp))
      if (strstr(buf, "%%EOF"))
        ++pdf->n_xrefs;

    /* Load in the start/end positions */
    ver = 1;
    fseek(fp, 0, SEEK_SET);
    pdf->xrefs = calloc(1, sizeof(xref_t) * pdf->n_xrefs);
    for (i=0; i<pdf->n_xrefs; i++)
    {
        while (fgets(buf, 256, fp))
          if ((c = strstr(buf, "%%EOF")))
            break;
        
        /* Seek to %%EOF */
        fseek(fp, c - buf - strlen(buf), SEEK_CUR);
        pos = ftell(fp);

        /* Rewind until we find end of "startxref" */
        pos_count = 0;
        while ((x = fgetc(fp)) != 'f')
          fseek(fp, pos - (++pos_count), SEEK_SET);
        
        /* Suck in end of "startxref" to start of %%EOF */
        memset(buf, 0, sizeof(buf));
        fread(buf, 1, pos_count, fp);
        c = buf;
        while (*c == ' ' || *c == '\n' || *c == '\r')
          ++c;
    
        /* xref start position */
        pdf->xrefs[i].start = atol(c);

        /* xref end position */
        pos = ftell(fp);
        fseek(fp, pdf->xrefs[i].start, SEEK_SET);
        while (fgets(buf, 256, fp))
          if ((c = strstr(buf, "%%EOF")))
            break;

        fseek(fp, c - buf - strlen(buf), SEEK_CUR);
        pdf->xrefs[i].end = ftell(fp);

        /* Look for next EOF and xref data */
        fseek(fp, pos, SEEK_SET);
        if (!is_valid_xref(fp, pdf, &pdf->xrefs[i]))
        {
            memset(&pdf->xrefs[i], 0, sizeof(xref_t));
            continue;
        }
        else
          pdf->xrefs[i].version = ver++;

        /*  Load the entries from the xref */
        load_xref_entries(fp, &pdf->xrefs[i]);
    }

    /* Ok now we have all xref data.  Go through those versions of the 
     * PDF and try to obtain creator information
     */
    load_creator(fp, pdf);

    return pdf->n_xrefs;
}


/* Load page information */
void pdf_load_pages_kids(FILE *fp, pdf_t *pdf)
{
    int     i, id, dummy;
    char   *buf, *c;
    long    start, sz;

    start = ftell(fp);

    /* Load all kids for all xref tables (versions) */
    for (i=0; i<pdf->n_xrefs; i++)
    {
        if (pdf->xrefs[i].version)
        {
            fseek(fp, pdf->xrefs[i].start, SEEK_SET);
            while (fgetc(fp) != 't')
                ; /* Iterate to trailer */

            /* Get root catalog */
            sz = pdf->xrefs[i].end - ftell(fp);
            buf = malloc(sz + 1);
            fread(buf, 1, sz, fp);
            buf[sz] = '\0';
            if (!(c = strstr(buf, "/Root")))
            {
                free(buf);
                continue;
            }

            /* Jump to catalog (root) */
            id = atoi(c + strlen("/Root") + 1);
            free(buf);
            buf = get_object(fp, id, &pdf->xrefs[i], NULL, &dummy);
            if (!buf || !(c = strstr(buf, "/Pages")))
            {
                free(buf);
                continue;
            }

            /* Start at the first Pages obj and get kids */
            id = atoi(c + strlen("/Pages") + 1);
            load_kids(fp, id, &pdf->xrefs[i]);
            free(buf); 
        }
    }
            
    fseek(fp, start, SEEK_SET);
}


char pdf_get_object_status(
    const pdf_t *pdf,
    int          xref_idx,
    int          entry_idx)
{
    int           i, j;
    xref_entry_t *prev, *curr;

    curr = &pdf->xrefs[xref_idx].entries[entry_idx];

    /* Deleted (freed) */
    if (curr->f_or_n == 'f')
      return 'D';

    /* Get previous entry */
    prev = NULL;
    for (i=xref_idx-1; i>-1; i--)
    {
        for (j=0; j<pdf->xrefs[i].n_entries; j++)
        {
            if (pdf->xrefs[i].entries[j].obj_id == curr->obj_id)
            {
                prev = &pdf->xrefs[i].entries[j];
                break;
            }
        }
    }

    /* Added in place of a previously freed id */
    if (!prev || ((prev->f_or_n == 'f') && (curr->f_or_n == 'n')))
      return 'A';

    /* Modified */
    else if (prev->offset != curr->offset)
      return 'M';
    
    return '?';
}


void pdf_zero_object(
    FILE        *fp,
    const pdf_t *pdf,
    int          xref_idx,
    int          entry_idx)
{
    int           i;
    char         *obj;
    size_t        obj_sz;
    xref_entry_t *entry;

    entry = &pdf->xrefs[xref_idx].entries[entry_idx];
    fseek(fp, entry->offset, SEEK_SET);

    /* Get object and size */
    obj = get_object(fp, entry->obj_id, &pdf->xrefs[xref_idx], NULL, NULL);
    i = obj_sz = 0;
    while (strncmp((++i)+obj, "endobj", 6))
      ++obj_sz;

    if (obj_sz)
      obj_sz += strlen("endobj") + 1;

    /* Zero object */
    for (i=0; i<obj_sz; i++)
      fputc('0', fp);

    free(obj);
}


/* Call pdf_creator_delete to properlly release memory */
pdf_creator_t *pdf_get_creator(FILE *fp, const pdf_t *pdf)
{
    /* TODO */
    return NULL;
}


/* Output information per version */
void pdf_summarize(
    FILE        *fp,
    const pdf_t *pdf,
    const char  *name,
    pdf_flag_t   flags)
{
    int   i, j, page, n_versions;
    FILE *dst, *out;
    char *dst_name, *c;

    dst_name = NULL;
    dst = NULL;

    if (name)
    {
        dst_name = malloc(strlen(name) * 2 + 16);
        sprintf(dst_name, "%s/%s", name, name);

        if ((c = strrchr(dst_name, '.')) && (strncmp(c, ".pdf", 4) == 0))
          *c = '\0';

        strcat(dst_name, ".summary");
        if (!(dst = fopen(dst_name, "w")))
        {
            ERR("Could not open file '%s' for writing\n", dst_name);
            return;
        }
    }
    
    /* Send output to file or stdout */
    out = (dst) ? dst : stdout;

    /* Count verions */
    n_versions = 0;
    for (i=0; i<pdf->n_xrefs; i++)
      if (pdf->xrefs[i].version)
        ++n_versions;

    /* Compare each object (if we dont have xref streams) */
    for (i=0; !(const int)pdf->has_xref_streams && i<pdf->n_xrefs; i++)
    {
        if (pdf->xrefs[i].version)
        {
            if (flags & PDF_FLAG_QUIET)
              continue;

            for (j=0; j<pdf->xrefs[i].n_entries; j++)
            {
                if (!pdf->xrefs[i].entries[j].obj_id)
                  continue;

                fprintf(out,
                        "%s: --%c-- Version %d -- Object %d (%s)",
                        pdf->name,
                        pdf_get_object_status(pdf, i, j),
                        pdf->xrefs[i].version,
                        pdf->xrefs[i].entries[j].obj_id,
                        get_type(fp, pdf->xrefs[i].entries[j].obj_id,
                                 &pdf->xrefs[i]));

                page = get_page(pdf->xrefs[i].entries[j].obj_id,
                                &pdf->xrefs[i]);

                if (page)
                  fprintf(out, " Page(%d)\n", page);
                else
                  fprintf(out, "\n");
            }
        }
    }

    /* Trailing summary */
    if (!(flags & PDF_FLAG_QUIET))
    {
        /* Let the user know that we cannot we print a per-object summary.
         * If we have a 1.5 PDF using streams for xref, we have not objects
         * to display, so let the user know whats up.
         */
        if (pdf->has_xref_streams)
           fprintf(out,
               "%s: This PDF contains cross reference streams.\n"
               "%s: An object summary is not available.\n",
               pdf->name,
               pdf->name);

        fprintf(out,
                "---------- %s ----------\n"
                "Versions: %d\n", 
                pdf->name,
                n_versions);

        if (!pdf->has_xref_streams)
          for (i=0; i<pdf->n_xrefs; i++)
            if (pdf->xrefs[i].version)
              fprintf(out,
                      "Version %d -- %d objects\n",
                      pdf->xrefs[i].version, 
                      pdf->xrefs[i].n_entries);
    }
    else /* Quiet output */
      fprintf(out, "%s: %d\n", pdf->name, n_versions);

    if (dst)
    {
        fclose(dst);
        free(dst_name);
    }
}


/* Checks if the xref is valid and sets 'is_stream' flag if the xref is a
 * stream (PDF 1.5 or higher) */
static int is_valid_xref(FILE *fp, pdf_t *pdf, xref_t *xref)
{
    int   is_valid;
    long  start;
    char *c, buf[16];

    is_valid = 0;
    start = ftell(fp);
    fseek(fp, xref->start, SEEK_SET);
    fgets(buf, 16, fp);

    if (strncmp(buf, "xref", strlen("xref")) == 0)
      is_valid = 1;

    /* PDF v1.5 + allows for xref data to be stored in streams vs plaintext */
    else
    {  
        fseek(fp, xref->start, SEEK_SET);
        c = get_object_from_here(fp, NULL, &xref->is_stream);

        if (c && xref->is_stream)
        {
            free(c);
            pdf->has_xref_streams = 1;
            is_valid = 1;
        }
    }

    fseek(fp, start, SEEK_SET);
    return is_valid;
}


static void load_xref_entries(FILE *fp, xref_t *xref)
{
    if (xref->is_stream)
      load_xref_from_stream(fp, xref);
    else
      load_xref_from_plaintext(fp, xref);
}


static void load_xref_from_plaintext(FILE *fp, xref_t *xref)
{
    int  i, buf_idx, obj_id, added_entries;
    char c, buf[21];
    long start, pos;

    start = ftell(fp);

    /* Get number of entries */
    pos = xref->end;
    fseek(fp, pos, SEEK_SET);
    while (ftell(fp) != 0)
      if (fgetc(fp) == '/' && fgetc(fp) == 'S')
        break;
      else
        fseek(fp, --pos, SEEK_SET);

    fread(buf, 1, 21, fp);
    xref->n_entries = atoi(buf + strlen("ize "));
    xref->entries = calloc(1, xref->n_entries * sizeof(struct _xref_entry));

    /* Load entry data */
    obj_id = 0;
    fseek(fp, xref->start + strlen("xref"), SEEK_SET);
    added_entries = 0;
    for (i=0; i<xref->n_entries; i++)
    {
        /* Skip */
        c = fgetc(fp);
        while (c == '\n' || c == '\r')
          c = fgetc(fp);

        buf_idx = 0;
        while (c != '\n' && c != '\r')
        {
            buf[buf_idx++] = c;
            c = fgetc(fp);
        }
        buf[buf_idx] = '\0';

        /* Went to far and hit start of trailer */
        if (strchr(buf, 't'))
          break;

        /* Entry or object id */
        if (strlen(buf) > 17)
        {
            xref->entries[i].obj_id = obj_id++;
            xref->entries[i].offset = atol(strtok(buf, " "));
            xref->entries[i].gen_num = atoi(strtok(NULL, " "));
            xref->entries[i].f_or_n = buf[17];
            ++added_entries;
        }
        else
        {
            obj_id = atoi(buf);
            --i;
        }
    }

    xref->n_entries = added_entries;
    fseek(fp, start, SEEK_SET);
}


/* Load an xref table from a stream (PDF v1.5 +) */
static void load_xref_from_stream(FILE *fp, xref_t *xref)
{
    long    start;
    int     is_stream;
    char   *stream;
    size_t  size;

    start = ftell(fp);
    fseek(fp, xref->start, SEEK_SET);

    stream = NULL;
    stream = get_object_from_here(fp, &size, &is_stream);
    fseek(fp, start, SEEK_SET);

    /* TODO: decode and analyize stream */
    free(stream);
    return;
}


static pdf_creator_t *new_creator(int *n_elements)
{
    pdf_creator_t *daddy;

    static const pdf_creator_t creator_template[] = 
    {
        {"Title",        ""},
        {"Author",       ""},
        {"Subject",      ""},
        {"Keywords",     ""},
        {"Creator",      ""},
        {"Producer",     ""},
        {"CreationDate", ""},
        {"ModDate",      ""},
        {"Trapped",      ""},
    };

    daddy = malloc(sizeof(creator_template));
    memcpy(daddy, creator_template, sizeof(creator_template));

    if (n_elements)
      *n_elements = sizeof(creator_template) / sizeof(creator_template[0]);

    return daddy;
}


#define END_OF_TRAILER(_c, _st, _fp) \
{                                    \
    if (_c == '>')                   \
    {                                \
        fseek(_fp, _st, SEEK_SET);   \
        return;                      \
    }                                \
}
static void load_creator(FILE *fp, pdf_t *pdf)
{
    int    i, buf_idx;
    char   c, *buf, obj_id_buf[32];
    long   start;
    size_t sz;

    start = ftell(fp);

    /* For each PDF version */
    for (i=0; i<pdf->n_xrefs; ++i)
    {
        if (!pdf->xrefs[i].version)
          continue;

        /* Find trailer */
        fseek(fp, pdf->xrefs[i].start, SEEK_SET);
        while (fgetc(fp) != 't')
            ; /* Iterate to "trailer" */

        /* Look for "<< ....... /Info ......" */
        while ((c = fgetc(fp)) != '>')
          if ((c == '/') && (fgetc(fp) == 'I') && ((fgetc(fp) == 'n')))
            break;

        /* Could not find /Info in trailer */
        END_OF_TRAILER(c, start, fp);

        while (!isspace(c = fgetc(fp)) && (c != '>'))
            ; /* Iterate to first white space /Info<space><data> */

        /* No space between /Info and it's data */
        END_OF_TRAILER(c, start, fp);

        while (isspace(c = fgetc(fp)) && (c != '>'))
            ; /* Iterate right on top of first non-whitespace /Info data */

        /* No data for /Info */
        END_OF_TRAILER(c, start, fp);

        /* Get obj id as number */
        buf_idx = 0;
        obj_id_buf[buf_idx++] = c;
        while (!isspace(c = fgetc(fp)) && (c != '>'))
          obj_id_buf[buf_idx++] = c;

        END_OF_TRAILER(c, start, fp);

        obj_id_buf[buf_idx] = '\0';
        buf = get_object(fp, atoll(obj_id_buf), &pdf->xrefs[i], &sz, NULL);
        load_creator_from_buf(&pdf->xrefs[i], buf);
        free(buf);
    }

    fseek(fp, start, SEEK_SET);
}


static void load_creator_from_buf(xref_t *xref, const char *buf)
{
    int   is_xml;
    char *c;

    if (!buf)
      return;

    /* Check to see if this is xml or old-school */
    if ((c = strstr(buf, "/Type")))
      while (*c && !isspace(*c))
        ++c;

    /* Probably "Metadata" */
    is_xml = 0;
    if (c && (*c == 'M'))
      is_xml = 1;

    /* Is the buffer XML(PDF 1.4+) or old format? */
    if (is_xml)
      load_creator_from_xml(xref, buf);
    else
      load_creator_from_old_format(xref, buf);
}


static void load_creator_from_xml(xref_t *xref, const char *buf)
{
    /* TODO */
}


static void load_creator_from_old_format(xref_t *xref, const char *buf)
{
    int            i, n_eles, length;
    char          *c;
    pdf_creator_t *info;

    info = new_creator(&n_eles);

    for (i=0; i<n_eles; ++i)
    {
        if (!(c = strstr(buf, info[i].key)))
          continue;

        /* Find the value (skipping whitespace) */
        c += strlen(info[i].key);
        while (isspace(*c))
          ++c;

        /* Find the end of the value */
        length = 0;
        while (c && ((*c != '\r') && (*c != '\n')))
        {
            ++c;
            ++length;
        }

        c -= length;
        length = (length > KV_MAX_VALUE_LENGTH) ? KV_MAX_VALUE_LENGTH : length;
        strncpy(info[i].value, c, length);
        info[i].value[length] = '\0';
    }

    xref->creator = info;
    xref->n_creator_entries = n_eles;
}


/* Returns object data at the start of the file pointer
 * This interfaces to 'get_object'
 */
static char *get_object_from_here(FILE *fp, size_t *size, int *is_stream)
{
    long         start;
    char         buf[256];
    int          obj_id;
    xref_t       xref;
    xref_entry_t entry;

    start = ftell(fp);

    /* Object ID */
    memset(buf, 0, 256);
    fread(buf, 255, 1, fp);
    if (!(obj_id = atoi(buf)))
    {
        fseek(fp, start, SEEK_SET);
        return NULL;
    }
    
    /* Create xref entry to pass to the get_object routine */
    memset(&entry, 0, sizeof(xref_entry_t));
    entry.obj_id = obj_id;
    entry.offset = start;

    /* Xref and single entry for the object we want data from */
    memset(&xref, 0, sizeof(xref_t));
    xref.n_entries = 1;
    xref.entries = &entry;

    fseek(fp, start, SEEK_SET);
    return get_object(fp, obj_id, &xref, size, is_stream);
}


static char *get_object(
    FILE         *fp,
    int           obj_id,
    const xref_t *xref,
    size_t       *size,
    int          *is_stream)
{
    static const int    blk_sz = 256;
    int                 i, total_sz, read_sz, n_blks, search, stream;
    size_t              obj_sz;
    char               *c, *data;
    long                start;
    const xref_entry_t *entry;

    if (size)
      *size = 0;

    if (is_stream)
      *is_stream = 0;

    start = ftell(fp);

    /* Find object */
    entry = NULL;
    for (i=0; i<xref->n_entries; i++)
      if (xref->entries[i].obj_id == obj_id)
      {
          entry = &xref->entries[i];
          break;
      }

    if (!entry)
      return NULL;

    /* Jump to object start */
    fseek(fp, entry->offset, SEEK_SET);

    /* Initial allocate */
    obj_sz = 0;    /* Bytes in object */
    total_sz = 0;  /* Bytes read in   */
    n_blks = 1;
    data = malloc(blk_sz * n_blks);
    memset(data, 0, blk_sz * n_blks);

    /* Suck in data */
    stream = 0;
    while ((read_sz = fread(data+total_sz, 1, blk_sz-1, fp)) && !ferror(fp))
    {
        total_sz += read_sz;

        *(data + total_sz) = '\0';

        if (total_sz + blk_sz >= (blk_sz * n_blks))
          data = realloc(data, blk_sz * (++n_blks));

        search = total_sz - read_sz;
        if (search < 0)
          search = 0;

        if ((c = strstr(data + search, "endobj")))
        {
            *(c + strlen("endobj") + 1) = '\0';
            obj_sz = (void *)strstr(data + search, "endobj") - (void *)data;
            obj_sz += strlen("endobj") + 1;
            break;
        }
        else if (strstr(data, "stream"))
          stream = 1;
    }

    clearerr(fp);
    fseek(fp, start, SEEK_SET);

    if (size)
      *size = obj_sz;
            
    if (is_stream)
      *is_stream = stream;

    return data;
}


static void add_kid(int id, xref_t *xref)
{
    /* Make some space */
    if (((xref->n_kids + 1) * KID_SIZE) > (xref->n_kids_allocs*KIDS_PER_ALLOC))
      xref->kids = realloc(
          xref->kids, (++xref->n_kids_allocs)*(KIDS_PER_ALLOC * KID_SIZE));

    xref->kids[xref->n_kids++] = id;
}


/* Recursive */
static void load_kids(FILE *fp, int pages_id, xref_t *xref)
{
    int   dummy, buf_idx, kid_id;
    char *data, *c, buf[32];

    /* Get kids */
    data = get_object(fp, pages_id, xref, NULL, &dummy);
    if (!data || !(c = strstr(data, "/Kids")))
    {
        free(data);
        return;
    }
    
    c = strchr(c, '[');
    buf_idx = 0;
    memset(buf, 0, sizeof(buf));
    while (*(++c) != ']')
    {
        if (isdigit(*c) || (*c == ' '))
          buf[buf_idx++] = *c;
        else if (isalpha(*c))
        {
            kid_id = atoi(buf);
            add_kid(kid_id, xref);
            buf_idx = 0;
            memset(buf, 0, sizeof(buf));

            /* Check kids of kid */
            load_kids(fp, kid_id, xref);
        }
        else if (*c == ']')
          break;
    }

    free(data);
}


static const char *get_type(FILE *fp, int obj_id, const xref_t *xref)
{
    int          is_stream;
    char        *c, *obj, *endobj;
    static char  buf[32];
    long         start;

    start = ftell(fp);

    if (!(obj = get_object(fp, obj_id, xref, NULL, &is_stream)) || 
        is_stream                                               ||
        !(endobj = strstr(obj, "endobj")))
    {
        free(obj);
        fseek(fp, start, SEEK_SET);

        if (is_stream)
          return "Stream";
        else
          return "Unknown";
    }

    /* Get the Type value (avoiding font names like Type1) */
    c = obj;
    while ((c = strstr(c, "/Type")) && (c < endobj))
      if (isdigit(*(c + strlen("/Type"))))
      {
          ++c;
          continue;
      }
      else
        break;

    if (!c || (c && (c > endobj)))
    {
        free(obj);
        fseek(fp, start, SEEK_SET);
        return "Unknown";
    }

    /* Skip to first blank/whitespace */
    c += strlen("/Type");
    while (isspace(*c) || *c == '/')
      ++c;

    /* Return the value by storing it in static mem */
    memcpy(buf, c, (((c - obj) < sizeof(buf)) ? c - obj : sizeof(buf)));
    c = buf;
    while (!(isspace(*c) || *c=='/' || *c=='>'))
      ++c;
    *c = '\0';

    free(obj);
    fseek(fp, start, SEEK_SET);

    return buf;
}


static int get_page(int obj_id, const xref_t *xref)
{
    /* TODO */
    return 0;
    /*
    int i;

    for (i=0; i<xref->n_kids; i++)
      if (xref->kids[i] == obj_id)
        break;

    return i;
    */
}


static char *get_header(FILE *fp)
{
    long start;

    /* First 1024 bytes of doc must be header (1.7 spec pg 1102) */
    char *header;

    header = calloc(1, 1024);
    
    start = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    fread(header, 1024, 1, fp);
    fseek(fp, start, SEEK_SET);
    
    return header;
}
