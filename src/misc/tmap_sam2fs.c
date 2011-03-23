/* Copyright (C) 2010 Ion Torrent Systems, Inc. All Rights Reserved */
#include <stdlib.h>
#include <stdio.h>
#include <config.h>
#include <ctype.h>
#include <unistd.h>

#ifdef HAVE_LIBPTHREAD
#include <pthread.h>
#endif

#ifdef HAVE_SAMTOOLS
#include <sam.h>
#include <bam.h>
#endif

#include "../util/tmap_error.h"
#include "../util/tmap_alloc.h"
#include "../util/tmap_progress.h"
#include "../util/tmap_definitions.h"
#include "../util/tmap_sam_print.h"
#include "../io/tmap_file.h"
#include "../sw/tmap_fsw.h"
#include "../sw/tmap_sw.h"
#include "../map/tmap_map_util.h"
#include "tmap_sam2fs_aux.h"
#include "tmap_sam2fs.h"

#ifdef HAVE_SAMTOOLS

#ifdef HAVE_LIBPTHREAD
static pthread_mutex_t tmap_sam2fs_read_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t tmap_sam2fs_read_lock_low = 0;
#define TMAP_SAM2FS_THREAD_BLOCK_SIZE 1024
#endif

// from bam.h
extern char *bam_nt16_rev_table;

// from tmap_fsw.c
extern int32_t tmap_fsw_sm_short[];

static void
tmap_sam2fs_gen_ap(tmap_fsw_param_t *par, 
                   int32_t score_match, int32_t pen_mm, 
                   int32_t pen_gapo, int32_t pen_gape,
                   int32_t fscore, int32_t flow_offset)
{
    int32_t i;
    for(i=0;i<25;i++) {
        par->matrix[i] = -100 * pen_mm;
    }
    for(i=0;i<4;i++) {
        par->matrix[i*5+i] = 100 * score_match;
    }
    par->gap_open = 100 * pen_gapo;
    par->gap_ext = 100 * pen_gape;
    par->gap_end = 100 * pen_gape;
    par->fscore = 100 * fscore;
    par->row = 5;
    par->band_width = 50;
    par->offset = flow_offset;
}


#define TMAP_SAM2FS_FLOW_MATCH '|'
#define TMAP_SAM2FS_FLOW_INS '+'
#define TMAP_SAM2FS_FLOW_DEL '-'
#define TMAP_SAM2FS_FLOW_SNP 'S'
#define TMAP_SAM2FS_FLOW_PAD ' '

typedef struct {
    bam1_t **bams;
    int32_t buffer_length;
    int32_t tid;
    tmap_sam2fs_opt_t *opt;
} tmap_sam2fs_thread_data_t;

static inline int32_t
tmap_sam2fs_is_DNA(char c)
{
  if('a' == c || 'A' == c
     || 'c' == c || 'C' == c
     || 'g' == c || 'G' == c
     || 't' == c || 'T' == c
     || 'n' == c || 'N' == c) {
      return 1;
  }
  else {
      return 0;
  }
}

static inline void 
tmap_sam2fs_bam_alloc_data(bam1_t *bam, int size)
{
  if(bam->m_data < size) {
      bam->m_data = size;
      tmap_roundup32(bam->m_data); // from bam.h
      bam->data = (uint8_t*)realloc(bam->data, bam->m_data);
  }
}


static bam1_t *
tmap_sam2fs_copy_to_sam(bam1_t *bam_old, tmap_fsw_path_t *path, int32_t path_len, int32_t score, 
                        int32_t soft_clip_start, int32_t soft_clip_end, uint8_t path_strand)
{
  bam1_t *bam_new = NULL;
  int32_t i, j, unmapped;
  uint32_t *cigar;
  int32_t n_cigar;
  uint8_t *old_score;

  if(1 == path_strand) {
      tmap_fsw_path_t *tmp_path;

      // reverse path
      tmp_path = tmap_malloc(sizeof(tmap_fsw_path_t) * path_len, "tmp_path");
      for(i=0;i<path_len;i++) {
          tmp_path[i] = path[path_len-i-1];
      }

      bam_new = tmap_sam2fs_copy_to_sam(bam_old, tmp_path, path_len, score,
                                        soft_clip_start, soft_clip_end, 0);

      free(tmp_path);
      return bam_new;
  }

  bam_new = tmap_calloc(1, sizeof(bam1_t), "bam_new");
  bam_new->data_len = 0; //bam_new->m_data;
  
  // get the cigar
  cigar = tmap_fsw_path2cigar(path, path_len, &n_cigar, 1);

  // check if the read should be unmapped
  for(i=0,unmapped=1;1 == unmapped && i<n_cigar;i++) {
      switch(cigar[i] & 0xf) {
        case TMAP_FSW_FROM_M:
        case TMAP_FSW_FROM_HP_MINUS:
        case TMAP_FSW_FROM_I:
          unmapped = 0;
          break;
        default:
          break;
      }
  }

  // query name
  bam_new->core.l_qname = bam_old->core.l_qname;
  bam_new->data_len += bam_new->core.l_qname;
  tmap_sam2fs_bam_alloc_data(bam_new, bam_new->data_len);
  memcpy(bam1_qname(bam_new), bam1_qname(bam_old), bam_old->core.l_qname);

  // flag
  if(0 == unmapped) {
      bam_new->core.flag = bam_old->core.flag;
  }
  else {
      bam_new->core.flag = BAM_FUNMAP;
  }

  // tid, pos, qual
  if(0 == unmapped) {
      bam_new->core.tid = bam_old->core.tid;
      bam_new->core.pos = bam_old->core.pos; 
      if(0 < path[path_len-1].j) { // adjust the position
          bam_new->core.pos += path[path_len-1].j;
      }
      bam_new->core.qual = bam_old->core.qual; 
  }
  else {
      bam_new->core.tid = -1;
      bam_new->core.pos = -1;
      bam_new->core.qual = 0;
  }

  // no pairing information
  bam_new->core.mtid = -1;
  bam_new->core.mpos = -1;
  bam_new->core.isize = 0;

  // cigar
  if(0 == unmapped) {
      tmap_sam2fs_bam_alloc_data(bam_new, bam_new->data_len);
      // remove HP edits
      for(i=j=0;i<n_cigar;i++) {
          switch(cigar[i] & 0xf) {
            case TMAP_FSW_FROM_HP_PLUS: // deletion
              cigar[i] = ((cigar[i] >> 4) << 4) | TMAP_FSW_FROM_D;
              break;
            case TMAP_FSW_FROM_HP_MINUS: // insertion
              cigar[i] = ((cigar[i] >> 4) << 4) | TMAP_FSW_FROM_I;
              break;
            default:
              break;
          }
          if(0 < j && (cigar[j-1] & 0xf) == (cigar[i] & 0xf)) {
              cigar[j-1] = ((cigar[j-1] >> 4) + (cigar[i] >> 4)) << 4;
          }
          else {
              j++;
          }
      }
      n_cigar = j;
      // add soft-clipping
      if(0 < soft_clip_start) {
          cigar = tmap_realloc(cigar, sizeof(uint32_t) * (1 + n_cigar), "cigar"); 
          for(i=n_cigar-1;0<=i;i--) {
              cigar[i+1] = cigar[i];
          }
          cigar[0] = (soft_clip_start << 4) | BAM_CSOFT_CLIP;
          n_cigar++;
      }
      if(0 < soft_clip_end) {
          cigar = tmap_realloc(cigar, sizeof(uint32_t) * (1 + n_cigar), "cigar"); 
          cigar[n_cigar] = (soft_clip_end << 4) | BAM_CSOFT_CLIP;
          n_cigar++;
      }
      // allocate
      bam_new->core.n_cigar = n_cigar;
      bam_new->data_len += bam_new->core.n_cigar * sizeof(uint32_t);
      tmap_sam2fs_bam_alloc_data(bam_new, bam_new->data_len);
      // copy over
      for(i=0;i<bam_new->core.n_cigar;i++) {
          bam1_cigar(bam_new)[i] = cigar[i];
      }
  }
  else {
      bam_new->core.n_cigar = 0;
  }
  free(cigar);

  // seq
  bam_new->core.l_qseq = bam_old->core.l_qseq;
  bam_new->data_len += (bam_new->core.l_qseq + 1)/2;
  tmap_sam2fs_bam_alloc_data(bam_new, bam_new->data_len);
  for(i=0;i<(bam_new->core.l_qseq+1)/2;i++) {
      bam1_seq(bam_new)[i] = bam1_seq(bam_old)[i];
  }

  // qualities
  bam_new->data_len += bam_new->core.l_qseq;
  tmap_sam2fs_bam_alloc_data(bam_new, bam_new->data_len);
  for(i=0;i<bam_new->core.l_qseq;i++) {
      bam1_qual(bam_new)[i] = bam1_qual(bam_old)[i];
  }

  // copy over auxiliary data
  bam_new->data_len += bam_old->l_aux;
  tmap_sam2fs_bam_alloc_data(bam_new, bam_new->data_len);
  for(i=0;i<bam_old->l_aux;i++) {
      bam1_aux(bam_new)[i] = bam1_aux(bam_old)[i];
  }
  bam_new->l_aux = bam_old->l_aux;

  // score
  old_score = bam_aux_get(bam_new, "AS");
  if(NULL == old_score) {
      bam_aux_append(bam_new, "AS", 'i', sizeof(uint32_t), (uint8_t*)(&score));
  }
  else {
      bam_aux_del(bam_new, old_score);
      bam_aux_append(bam_new, "AS", 'i', sizeof(uint32_t), (uint8_t*)(&score));
  }

  // destroy the old bam
  bam_destroy1(bam_old);

  return bam_new;
}

static bam1_t *
tmap_sam2fs_aux(bam1_t *bam, char *flow_order, int32_t score_match, int32_t pen_mm, int32_t pen_gapo, int32_t pen_gape,
                int32_t fscore, int32_t flow_offset, 
                int32_t softclip_type, int32_t output_type, int32_t output_newlines, int32_t j_type)
{
  int32_t i, j, k, l;

  uint8_t *md_data = NULL;
  char *md = NULL;
  uint32_t md_i = 0;
  char *ref, *read, *aln;

  uint32_t aln_len = 0;
  uint32_t *cigar = NULL;
  uint8_t *bam_seq = NULL;

  char *read_bases = NULL;
  int32_t read_bases_len = 0, read_bases_mem = 256;
  char *ref_bases = NULL;
  int32_t ref_bases_len = 0, ref_bases_mem = 64;
  int32_t tmp_read_bases_len, tmp_ref_bases_len;
  int32_t tmp_read_bases_offset, tmp_ref_bases_offset;
  uint8_t *flow_order_tmp = NULL;
  int32_t flow_order_len = 0;

  int32_t soft_clip_start=0, soft_clip_end=0;

  int32_t flow_len;
  uint8_t *base_calls = NULL;
  uint16_t *flowgram = NULL;
  tmap_fsw_path_t *path = NULL;
  int32_t path_len;
  tmap_fsw_param_t param;
  int64_t score;
  tmap_fsw_flowseq_t *flowseq = NULL;
  uint8_t strand;
  int32_t matrix[25];

  char separator;

  separator = (0 == output_newlines) ? '\t' : '\n';

  // set the alignment parameters
  param.matrix = matrix;
  tmap_sam2fs_gen_ap(&param, score_match, pen_mm, pen_gapo, pen_gape, fscore, flow_offset);

  if(BAM_FUNMAP & bam->core.flag) {
      return bam;
  }

  strand = (BAM_FREVERSE & bam->core.flag) ? 1 : 0;

  // get the MD tag
  if(NULL == (md_data = bam_aux_get(bam, "MD"))) {
      tmap_error("MD tag is missing", Exit, OutOfRange);
  }
  md = bam_aux2Z(md_data);

  // cigar
  cigar = bam1_cigar(bam);

  // get the alignment length
  for(i=aln_len=0;i<bam->core.n_cigar;i++) {
      aln_len += (cigar[i] >> 4);
  }
  if(0 == aln_len) {
      tmap_error("zero alignment length", Exit, OutOfRange);
  }

  // get soft clipping at the start/end of the sequence
  if((BAM_CSOFT_CLIP & cigar[0])) {
      soft_clip_start  = (cigar[0] >> 4);
  }
  if(1 < bam->core.n_cigar && (BAM_CSOFT_CLIP & cigar[bam->core.n_cigar-1])) {
      soft_clip_end = (cigar[bam->core.n_cigar-1] >> 4);
  }

  /*
     if(0 < soft_clip_start || 0 < soft_clip_end) {
     tmap_error("Soft clipping currently not supported", Exit, OutOfRange);
     }
     */

  // get the read bases
  bam_seq = bam1_seq(bam);
  read_bases = tmap_calloc(1+bam->core.l_qseq, sizeof(char), "read_bases");
  for(i=soft_clip_start;i<bam->core.l_qseq-soft_clip_end;i++) {
      read_bases[i-soft_clip_start] = bam_nt16_rev_table[bam1_seqi(bam_seq, i)]; 
  }
  read_bases[i]='\0';
  read_bases_len = bam->core.l_qseq - soft_clip_start - soft_clip_end; 
  read_bases_mem = read_bases_len + 1;

  // get the reference bases
  ref_bases = tmap_calloc(ref_bases_mem, sizeof(char), "read_bases");

  // pre-process using the cigar array
  for(i=j=k=0;i<bam->core.n_cigar;i++) {
      int32_t op, op_len;
      op = (cigar[i] & 0xf);
      op_len = (cigar[i] >> 4);

      switch(op) {
        case BAM_CMATCH:
          // copy over these bases
          while(ref_bases_mem <= ref_bases_len + op_len + 1) {
              ref_bases_mem <<= 1;
              ref_bases = tmap_realloc(ref_bases, sizeof(char)*ref_bases_mem, "ref_bases");
          }
          for(l=0;l<op_len;l++) {
              ref_bases[k+l] = read_bases[j+l];
          }
          j += op_len; 
          k += op_len; 
          ref_bases_len += op_len;
          break;
        case BAM_CSOFT_CLIP:
          // Note: these have already been removed from read_bases
          break;
        case BAM_CINS:
          j += op_len;
          break;
        case BAM_CREF_SKIP:
        case BAM_CDEL:
        case BAM_CHARD_CLIP:
        case BAM_CPAD:
          // ignore
          break;
        default:
          tmap_error("unknown cigar operator", Exit, OutOfRange);
          break;
      }
  }

  // fill in with the MD array
  for(md_i=i=0;md_i<strlen(md);) {
      if('0' <= md[md_i] && md[md_i] <= '9') { // 0-9, matches
          l = atoi(md + md_i);
          i += l;
          while(md_i < strlen(md) && '0' <= md[md_i] && md[md_i] <= '9') { // 0-9
              md_i++; // skip over integers
          }
      }
      else if('^' == md[md_i]) { // deletion from the reference
          // how many bases are deleted?
          md_i++; // skip over '^'
          l=0;
          while(md_i+l < strlen(md) && 1 == tmap_sam2fs_is_DNA(md[md_i+l])) {
              l++;
          }
          // reallocate
          while(ref_bases_mem <= ref_bases_len + l + 1) { // more memory please
              ref_bases_mem <<= 1;
              ref_bases = tmap_realloc(ref_bases, sizeof(char)*ref_bases_mem, "ref_bases");
          }
          // shift up
          for(j=ref_bases_len-1;i<=j;j--) {
              ref_bases[j+l] = ref_bases[j];
          }
          // fill in
          for(j=0;j<l;j++) {
              ref_bases[i+j] = md[md_i+j];
          }
          md_i += l;
          i += l;
          ref_bases_len += l;
      }
      else if(1 == tmap_sam2fs_is_DNA(md[md_i])) { // SNP
          ref_bases[i] = md[md_i];
          i++;
          md_i++;
      }
      else {
          tmap_error("could not parse the MD tag", Exit, OutOfRange);
      }
  }
  ref_bases[i]='\0';

  // reverse compliment if necessary
  if(1 == strand) {
      tmap_reverse_compliment(ref_bases, ref_bases_len);
      tmap_reverse_compliment(read_bases, read_bases_len);
  }

  /*
  fprintf(stderr, "ref_bases_len=%d\nref_bases=%s\n", ref_bases_len, ref_bases);
  fprintf(stderr, "read_bases_len=%d\nread_bases=%s\n", read_bases_len, read_bases);
  */

  // DNA to integer
  for(i=0;i<read_bases_len;i++) {
      read_bases[i] = tmap_nt_char_to_int[(int)read_bases[i]];
  }
  for(i=0;i<ref_bases_len;i++) {
      ref_bases[i] = tmap_nt_char_to_int[(int)ref_bases[i]];
  }
  flow_order_len = strlen(flow_order);
  flow_order_tmp = tmap_malloc(sizeof(uint8_t) * flow_order_len, "flow_order_tmp");
  for(i=0;i<flow_order_len;i++) {
      flow_order_tmp[i] = tmap_nt_char_to_int[(int)flow_order[i]];
  }

  // get the flow length
  flow_len = 0;
  for(i=j=0;i<read_bases_len;i++) {
      while(flow_order_tmp[j] != read_bases[i]) {
          flow_len++;
          j = (1+j) % flow_order_len;
      }
  }
  flow_len++; // the last flow
  // alloc
  base_calls = tmap_calloc(flow_len, sizeof(uint8_t), "base_calls");
  flowgram = tmap_calloc(flow_len, sizeof(uint16_t), "base_calls");
  // copy the # of flows
  for(i=j=0;i<flow_len;i++) {
      if(flow_order_tmp[i % flow_order_len] == read_bases[j]) {
          k=j;
          while(j < read_bases_len 
                && read_bases[j] == read_bases[k]) {
              j++;
          }
          base_calls[i] = j-k;
          flowgram[i] = 100*(j-k);
      }
      else {
          base_calls[i] = 0;
          flowgram[i] = 0;
      }
  }

  // allocate the alignment path
  path = tmap_calloc(TMAP_FSW_MAX_PATH_LENGTH(ref_bases_len, flow_len, param.offset), sizeof(tmap_fsw_path_t), "path"); 

  // re-align 
  flowseq = tmap_fsw_flowseq_init(flow_order_tmp, flow_order_len, base_calls, flowgram, flow_len, -1, 0);
  score = INT32_MIN;
  switch(softclip_type) {
    case TMAP_MAP_UTIL_SOFT_CLIP_ALL:
      score = tmap_fsw_clipping_core((uint8_t*)ref_bases, ref_bases_len, flowseq, &param,
                                        1, 1, path, &path_len);
      break;
    case TMAP_MAP_UTIL_SOFT_CLIP_LEFT:
      score = tmap_fsw_clipping_core((uint8_t*)ref_bases, ref_bases_len, flowseq, &param,
                                        1, 0, path, &path_len);
      break;
    case TMAP_MAP_UTIL_SOFT_CLIP_RIGHT:
      score = tmap_fsw_clipping_core((uint8_t*)ref_bases, ref_bases_len, flowseq, &param,
                                        0, 1, path, &path_len);
      break;
    case TMAP_MAP_UTIL_SOFT_CLIP_NONE:
      score = tmap_fsw_clipping_core((uint8_t*)ref_bases, ref_bases_len, flowseq, &param,
                                        0, 0, path, &path_len);
      break;
    default:
      tmap_error("soft clipping type was not recognized", Exit, OutOfRange);
      break;
  }

  if(0 == path_len) {
      tmap_error("bug encountered", Exit, OutOfRange);
  }

  /*
  for(i=path_len-1;0<=i;i--) {
      fprintf(stderr, "i=%d path[i].i=%d path[i].j=%d path[i].ctype=%d\n",
              i, path[i].i, path[i].j, path[i].ctype);
  }
  */

  // Account for soft-clipping
  tmp_read_bases_len = 0;
  for(i=0;i<=path[0].i;i++) { // include the last flow
      tmp_read_bases_len += flowseq->base_calls[i];
  }
  tmp_read_bases_offset = 0;
  for(i=0;i<path[path_len-1].i;i++) { // do not include the first flow
      tmp_read_bases_offset += flowseq->base_calls[i]; 
  }
  tmp_read_bases_len -= tmp_read_bases_offset;
  if(path[path_len-1].j < 0) { // check if we start with an insertion in the read
      tmp_ref_bases_len = path[0].j + 1;
      tmp_ref_bases_offset = 0;
  }
  else {
      tmp_ref_bases_len = path[0].j - path[path_len-1].j + 1;
      tmp_ref_bases_offset = path[path_len-1].j;
  }
  /*
     fprintf(stderr, "tmp_read_bases_len=%d\ntmp_read_bases_offset=%d\n",
     tmp_read_bases_len, tmp_read_bases_offset);
     fprintf(stderr, "tmp_ref_bases_len=%d\ntmp_ref_bases_offset=%d\n",
     tmp_ref_bases_len, tmp_ref_bases_offset);
     */

  switch(output_type) {
    case TMAP_SAM2FS_OUTPUT_ALN:
      tmap_file_fprintf(tmap_file_stdout, "%s%c%c%c", bam1_qname(bam), separator, (1==strand) ? '-' : '+', separator);
      tmap_fsw_print_aln(tmap_file_stdout, score, path, path_len, flow_order_tmp, flow_order_len, 
                         (uint8_t*)ref_bases,
                         strand,
                         j_type,
                         separator);
      tmap_file_fprintf(tmap_file_stdout, "%c", separator);
      // TODO
      // what about clipping within a flow and ref?
      // TODO:
      // before the first referene base and after last reference flow should both be gaps!
      // similarly for the read
      tmap_sam2fs_aux_flow_align(tmap_file_stdout, 
                                 (uint8_t*)(read_bases + tmp_read_bases_offset), 
                                 tmp_read_bases_len,
                                 (uint8_t*)(ref_bases + tmp_ref_bases_offset),
                                 tmp_ref_bases_len,
                                 flow_order_tmp,
                                 flow_order_len,
                                 strand,
                                 separator);
      tmap_file_fprintf(tmap_file_stdout, "\n");
      break;
    case TMAP_SAM2FS_OUTPUT_SAM:
      soft_clip_start += tmp_read_bases_offset;
      soft_clip_end += read_bases_len - tmp_read_bases_len - tmp_read_bases_offset;

      // Account for soft-clipping
      tmp_read_bases_len = 0;
      for(i=0;i<=path[0].i;i++) { // include the last flow
          tmp_read_bases_len += flowseq->base_calls[i];
      }
      tmp_read_bases_offset = 0;
      for(i=0;i<path[path_len-1].i;i++) { // do not include the first flow
          tmp_read_bases_offset += flowseq->base_calls[i]; 
      }
      tmp_read_bases_len -= tmp_read_bases_offset;

      bam = tmap_sam2fs_copy_to_sam(bam, path, path_len, score, soft_clip_start, soft_clip_end, strand);

      if(bam->core.n_cigar > 0) {
          tmap_fsw_get_aln(path, path_len, flow_order_tmp, flow_order_len, (uint8_t*)ref_bases, 
                           strand,
                           &ref, &read, &aln, j_type);


          // do not worry about read fitting since tmap_fsw_get_aln handles this
          // above
          if(1 == strand) { // set it the forward strand of the reference
              tmap_reverse_compliment(ref, path_len);
              tmap_reverse_compliment(read, path_len);
          }
          tmap_sam_update_cigar_and_md(bam, ref, read, path_len);
          
          // free
          free(ref); free(read); free(aln); 
      }
      break;
  }

  // free memory
  tmap_fsw_flowseq_destroy_shallow(flowseq);
  free(base_calls);
  free(flowgram);
  free(flow_order_tmp);
  free(path);
  free(read_bases);
  free(ref_bases);

  return bam;
}

static void
tmap_sam2fs_aux_worker(bam1_t **bams, int32_t buffer_length, tmap_sam2fs_opt_t *opt)
{
  int32_t low = 0, high;

  while(low < buffer_length) {
#ifdef HAVE_LIBPTHREAD
      if(1 < opt->num_threads) {
          pthread_mutex_lock(&tmap_sam2fs_read_lock);

          // update bounds
          low = tmap_sam2fs_read_lock_low;
          tmap_sam2fs_read_lock_low += TMAP_SAM2FS_THREAD_BLOCK_SIZE;
          high = low + TMAP_SAM2FS_THREAD_BLOCK_SIZE;
          if(buffer_length < high) {
              high = buffer_length;
          }

          pthread_mutex_unlock(&tmap_sam2fs_read_lock);
      }
      else {
          high = buffer_length; // process all
      }
#else
      high = buffer_length; // process all
#endif
      while(low < high) {
          bams[low] = tmap_sam2fs_aux(bams[low], opt->flow_order, 
                                      opt->score_match, opt->pen_mm, opt->pen_gapo, opt->pen_gape,
                                      opt->fscore, opt->flow_offset, 
                                      opt->softclip_type, opt->output_type, opt->output_newlines,
                                      opt->j_type);
          low++;
      }
  }
}

static void *
tmap_sam2fs_aux_thread_worker(void *arg)
{
  tmap_sam2fs_thread_data_t *thread_data = (tmap_sam2fs_thread_data_t*)arg;
  tmap_sam2fs_aux_worker(thread_data->bams, thread_data->buffer_length, thread_data->opt);

  return arg;
}

static void
tmap_sam2fs_core(const char *fn_in, const char *sam_open_flags, tmap_sam2fs_opt_t *opt)
{
  int32_t i;
  samfile_t *fp_in = NULL;
  samfile_t *fp_out = NULL;
  bam1_t *b = NULL;
  bam1_t **bams = NULL;
  int32_t reads_queue_size;
  int32_t buffer_length, n_reads_processed = 0;

  fp_in = samopen(fn_in, sam_open_flags, 0);
  if(NULL == fp_in) tmap_error(fn_in, Exit, OpenFileError);

  switch(opt->output_type) {
    case TMAP_SAM2FS_OUTPUT_ALN:
      tmap_file_stdout = tmap_file_fdopen(fileno(stdout), "wb", TMAP_FILE_NO_COMPRESSION);
      break;
    case TMAP_SAM2FS_OUTPUT_SAM:
      fp_out = samopen("-", "wh", fp_in->header);
      break;
    case TMAP_SAM2FS_OUTPUT_BAM:
      fp_out = samopen("-", "wb", fp_in->header);
      break;
  }

  if(TMAP_SAM2FS_OUTPUT_SAM == opt->output_type
     || TMAP_SAM2FS_OUTPUT_BAM == opt->output_type) {
      // allocate the buffer
      if(-1 == opt->reads_queue_size) {
          reads_queue_size = 1;
      }
      else {
          reads_queue_size = opt->reads_queue_size;
      }
      bams = tmap_malloc(sizeof(bam1_t*)*reads_queue_size, "bams");

      tmap_progress_print("processing reads");
      do {
          // init
          for(i=0;i<reads_queue_size;i++) {
              bams[i] = bam_init1();
          }

          // read into the buffer
          for(i=buffer_length=0;i<reads_queue_size;i++) {
              if(samread(fp_in, bams[i]) <= 0) {
                  break;
              }
              buffer_length++;
          }

          // process
#ifdef HAVE_LIBPTHREAD
          int32_t num_threads = opt->num_threads;
          if(buffer_length < num_threads * TMAP_SAM2FS_THREAD_BLOCK_SIZE) {
              num_threads = 1 + (buffer_length / TMAP_SAM2FS_THREAD_BLOCK_SIZE);
          }
          if(1 == num_threads) {
              tmap_sam2fs_aux_worker(bams, buffer_length, opt);
          }
          else {
              pthread_attr_t attr;
              pthread_t *threads = NULL;
              tmap_sam2fs_thread_data_t *thread_data=NULL;

              pthread_attr_init(&attr);
              pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

              threads = tmap_calloc(num_threads, sizeof(pthread_t), "threads");
              thread_data = tmap_calloc(num_threads, sizeof(tmap_sam2fs_thread_data_t), "thread_data");
              tmap_sam2fs_read_lock_low = 0; // ALWAYS set before running threads 

              for(i=0;i<num_threads;i++) {
                  thread_data[i].bams = bams;
                  thread_data[i].buffer_length = buffer_length;
                  thread_data[i].tid = i;
                  thread_data[i].opt = opt;
                  if(0 != pthread_create(&threads[i], &attr, tmap_sam2fs_aux_thread_worker, &thread_data[i])) {
                      tmap_error("error creating threads", Exit, ThreadError);
                  }
              }
              for(i=0;i<num_threads;i++) {
                  if(0 != pthread_join(threads[i], NULL)) {
                      tmap_error("error joining threads", Exit, ThreadError);
                  }
              }

              free(threads);
              free(thread_data);

          }
#else
          tmap_sam2fs_aux_worker(bams, buffer_length, opt);
#endif

          // write to SAM/BAM if necessary
          tmap_progress_print("writing alignments");
          for(i=0;i<buffer_length;i++) {
              if(samwrite(fp_out, bams[i]) < 0) {
                  tmap_error(NULL, Exit, WriteFileError);
              }
          }

          // destroy
          for(i=0;i<reads_queue_size;i++) {
              bam_destroy1(bams[i]);
              bams[i] = NULL;
          }

          n_reads_processed += buffer_length;
          tmap_progress_print2("processed %d reads", n_reads_processed);
      } while(0 < buffer_length);

      // free
      free(bams);
  }
  else {
      b = bam_init1();
      while(0 < samread(fp_in, b)) { 
          // process 
          b = tmap_sam2fs_aux(b, opt->flow_order, 
                              opt->score_match, opt->pen_mm,
                              opt->pen_gapo, opt->pen_gape,
                              opt->fscore, opt->flow_offset, 
                              opt->softclip_type, opt->output_type, opt->output_newlines,
                              opt->j_type);
          // destroy the bam
          bam_destroy1(b);
          // reinitialize
          b = bam_init1();
      }
      bam_destroy1(b);
  }

  // close
  samclose(fp_in); 
  switch(opt->output_type) {
    case TMAP_SAM2FS_OUTPUT_ALN:
      tmap_file_fclose(tmap_file_stdout);
      break;
    case TMAP_SAM2FS_OUTPUT_SAM:
    case TMAP_SAM2FS_OUTPUT_BAM:
      samclose(fp_out);
      break;
  }
}

tmap_sam2fs_opt_t *
tmap_sam2fs_opt_init()
{
  tmap_sam2fs_opt_t *opt=NULL;

  opt = tmap_calloc(1, sizeof(tmap_sam2fs_opt_t), "opt");

  opt->flow_order = tmap_strdup("TACG");
  opt->score_match = TMAP_MAP_UTIL_SCORE_MATCH;
  opt->pen_mm = TMAP_MAP_UTIL_PEN_MM;
  opt->pen_gapo = TMAP_MAP_UTIL_PEN_GAPO;
  opt->pen_gape = TMAP_MAP_UTIL_PEN_GAPE;
  opt->fscore = TMAP_MAP_UTIL_FSCORE;
  opt->flow_offset = 1;
  opt->softclip_type = TMAP_MAP_UTIL_SOFT_CLIP_ALL;
  opt->output_type = TMAP_SAM2FS_OUTPUT_ALN;
  opt->output_newlines = 0;
  opt->j_type = TMAP_FSW_NO_JUSTIFY;
  opt->reads_queue_size = 65536; 
  opt->num_threads = 1;

  return opt;
}

void
tmap_sam2fs_opt_destroy(tmap_sam2fs_opt_t *opt)
{
  free(opt->flow_order);
  free(opt);
}

static int
usage(tmap_sam2fs_opt_t *opt)
{
  tmap_file_fprintf(tmap_file_stderr, "\n");
  tmap_file_fprintf(tmap_file_stderr, "Usage: %s sam2fs [options] <in.sam/in.bam>", PACKAGE);
  tmap_file_fprintf(tmap_file_stderr, "\n");
  tmap_file_fprintf(tmap_file_stderr, "Options (optional):\n");
  tmap_file_fprintf(tmap_file_stderr, "         -f          the flow order [%s]\n", opt->flow_order);
  tmap_file_fprintf(tmap_file_stderr, "         -A INT      score for a match [%d]\n", opt->score_match);
  tmap_file_fprintf(tmap_file_stderr, "         -M INT      the mismatch penalty [%d]\n", opt->pen_mm);
  tmap_file_fprintf(tmap_file_stderr, "         -O INT      the indel start penalty [%d]\n", opt->pen_gapo);
  tmap_file_fprintf(tmap_file_stderr, "         -E INT      the indel extend penalty [%d]\n", opt->pen_gape);
  tmap_file_fprintf(tmap_file_stderr, "         -X INT      the flow score penalty [%d]\n", opt->fscore);
  tmap_file_fprintf(tmap_file_stderr, "         -o INT      search for homopolymer errors +- offset during re-alignment [%d]\n",
                    opt->flow_offset);
  tmap_file_fprintf(tmap_file_stderr, "         -S          the input is a SAM file\n");
  tmap_file_fprintf(tmap_file_stderr, "         -g          the soft-clipping type [%d]\n", opt->softclip_type);
  tmap_file_fprintf(tmap_file_stderr, "                             0 - allow on the right and left portions of the read\n");
  tmap_file_fprintf(tmap_file_stderr, "                             1 - allow on the left portion of the read\n");
  tmap_file_fprintf(tmap_file_stderr, "                             2 - allow on the right portion of the read\n");
  tmap_file_fprintf(tmap_file_stderr, "                             3 - do not allow soft-clipping\n");
  tmap_file_fprintf(tmap_file_stderr, "         -t          the output type: 0-alignment 1-SAM 2-BAM [%d]\n", opt->output_type);
  tmap_file_fprintf(tmap_file_stderr, "         -N          use newline separators when outputting the alignments (-t 0 only)\n");
  tmap_file_fprintf(tmap_file_stderr, "         -l INT      indel justification type: 0 - none, 1 - 5' strand of the reference, 2 - 5' strand of the read [%d]\n", opt->j_type);
  tmap_file_fprintf(tmap_file_stderr, "         -q INT      the queue size for the reads (-1 disables) [%d]\n", opt->reads_queue_size);
  tmap_file_fprintf(tmap_file_stderr, "         -n INT      the number of threads [%d]\n", opt->num_threads);
  tmap_file_fprintf(tmap_file_stderr, "         -v          print verbose progress information\n");
  tmap_file_fprintf(tmap_file_stderr, "         -h          print this message\n");
  tmap_file_fprintf(tmap_file_stderr, "\n");

  return 1;
}

int
tmap_sam2fs_main(int argc, char *argv[])
{
  int c;
  tmap_sam2fs_opt_t *opt;
  char sam_open_flags[16] = "rb";

  opt = tmap_sam2fs_opt_init();

  while((c = getopt(argc, argv, "f:A:M:O:E:X:o:Sg:t:Nl:q:n:vh")) >= 0) {
      switch(c) {
        case 'f':
          strncpy(opt->flow_order, optarg, 4); break;
        case 'A':
          opt->score_match = atoi(optarg); break;
        case 'M':
          opt->pen_mm = atoi(optarg); break;
        case 'O':
          opt->pen_gapo = atoi(optarg); break;
        case 'E':
          opt->pen_gape = atoi(optarg); break;
        case 'X':
          opt->fscore = atoi(optarg); break;
        case 'o':
          opt->flow_offset = atoi(optarg); break;
        case 'S':
          strcpy(sam_open_flags, "r"); break;
        case 'g':
          opt->softclip_type = atoi(optarg); break;
        case 't':
          opt->output_type = atoi(optarg); break;
        case 'N':
          opt->output_newlines = 1; break;
        case 'l':
          opt->j_type = atoi(optarg); break;
        case 'q':
          opt->reads_queue_size = atoi(optarg); break;
        case 'n':
          opt->num_threads = atoi(optarg); break;
        case 'v':
          tmap_progress_set_verbosity(1); break;
        case 'h':
        default:
          return usage(opt);
      }
  }

  if(argc != optind+1 || 1 == argc) {
      return usage(opt);
  }
  else { // check command line options
      tmap_error_cmd_check_int(opt->score_match, 0, INT32_MAX, "-A");
      tmap_error_cmd_check_int(opt->pen_mm, 0, INT32_MAX, "-M");
      tmap_error_cmd_check_int(opt->pen_gapo, 0, INT32_MAX, "-O");
      tmap_error_cmd_check_int(opt->pen_gape, 0, INT32_MAX, "-E");
      tmap_error_cmd_check_int(opt->fscore, 0, INT32_MAX, "-X");
      tmap_error_cmd_check_int(opt->flow_offset, 0, INT32_MAX, "-o");
      tmap_error_cmd_check_int((int)strlen(opt->flow_order), 4, 4, "-f");
      tmap_error_cmd_check_int(opt->output_type, 0, 2, "-t");
      if(TMAP_SAM2FS_OUTPUT_ALN != opt->output_type) tmap_error_cmd_check_int(opt->output_newlines, 0, 0, "-N");
      if(-1 != opt->reads_queue_size) tmap_error_cmd_check_int(opt->reads_queue_size, 1, INT32_MAX, "-q");
      tmap_error_cmd_check_int(opt->num_threads, 1, INT32_MAX, "-n"); 
  }

  tmap_sam2fs_core(argv[optind], sam_open_flags, opt);

  tmap_sam2fs_opt_destroy(opt);

  tmap_progress_print2("terminating successfully");

  return 0;
}  
#endif
