/*
 *  ssw.c
 *
 *  Created by Mengyao Zhao on 6/22/10.
 *  Copyright 2010 Boston College. All rights reserved.
 *	Version 0.1.4
 *	Last revision by Mengyao Zhao on 03/06/12.
 *	New features: Combine files for api wrapping. 
 *
 */

#include <emmintrin.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ssw.h"

#ifdef __GNUC__
#define LIKELY(x) __builtin_expect((x),1)
#define UNLIKELY(x) __builtin_expect((x),0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

/* Convert the coordinate in the scoring matrix into the coordinate in one line of the band. */
#define set_u(u, w, i, j) { int x=(i)-(w); x=x>0?x:0; (u)=(j)-x+1; }

/* Convert the coordinate in the direction matrix into the coordinate in one line of the band. */
#define set_d(u, w, i, j, p) { int x=(i)-(w); x=x>0?x:0; x=(j)-x; (u)=x*3+p; }

/*! @function
  @abstract  Round an integer to the next closest power-2 integer.
  @param  x  integer to be rounded (in place)
  @discussion x will be modified.
 */
#define kroundup32(x) (--(x), (x)|=(x)>>1, (x)|=(x)>>2, (x)|=(x)>>4, (x)|=(x)>>8, (x)|=(x)>>16, ++(x))

typedef struct {
	uint16_t score;
	int32_t ref;	 //0-based position 
	int32_t read;    //alignment ending position on read, 0-based 
} alignment_end;

struct _profile{
	__m128i* profile_byte;	// 0: none
	__m128i* profile_word;	// 0: none
//	__m128i* reverse_byte;	// 0: none
//	__m128i* reverse_word;	// 0: none
	int8_t* read;
//	int8_t* rc_read;	// reverse complement sequence of the read, 0: none
	int8_t* mat;
	int32_t readLen;
	int32_t n;
};

/* Generate query profile rearrange query sequence & calculate the weight of match/mismatch. */
__m128i* qP_byte (int8_t* read_num,
				  int8_t* mat,
				  int32_t readLen,
				  int32_t n,	/* the edge length of the squre matrix mat */
				  uint8_t bias) { 
					
	int32_t
	segLen = (readLen + 15) / 16; /* Split the 128 bit register into 16 pieces. 
								     Each piece is 8 bit. Split the read into 16 segments. 
								     Calculat 16 segments in parallel.
								   */
	__m128i* vProfile = (__m128i*)calloc(n * segLen, sizeof(__m128i));
	int8_t* t = (int8_t*)vProfile;
	int32_t nt, i, j;
	int32_t segNum;
	
	/* Generate query profile rearrange query sequence & calculate the weight of match/mismatch */
	for (nt = 0; LIKELY(nt < n); nt ++) {
		for (i = 0; i < segLen; i ++) {
			j = i; 
			for (segNum = 0; LIKELY(segNum < 16) ; segNum ++) {
				*t++ = j>= readLen ? 0 : mat[nt * n + read_num[j]] + bias;
				j += segLen;
			}
		}
	}

	return vProfile;
}

/* Striped Smith-Waterman
   Record the highest score of each reference position. 
   Return the alignment score and ending position of the best alignment, 2nd best alignment, etc. 
   Gap begin and gap extension are different. 
   wight_match > 0, all other weights < 0.
   The returned positions are 0-based.
 */ 
alignment_end* sw_sse2_byte (int8_t* ref,
							 int8_t ref_dir,	// 0: forward ref; 1: reverse ref
							 int32_t refLen,
							 int32_t readLen, 
							 uint8_t weight_insertB, /* will be used as - */
							 uint8_t weight_insertE, /* will be used as - */
						     uint8_t weight_deletB,  /* will be used as - */
							 uint8_t weight_deletE,  /* will be used as - */
							 __m128i* vProfile,
							 uint8_t terminate,	/* the best alignment score: used to terminate 
												   the matrix calculation when locating the 
												   alignment beginning point. If this score 
												   is set to 0, it will not be used */
	 						 uint8_t bias) {         /* Shift 0 point to a positive value. */

#define max16(m, vm) (vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 8)); \
					  (vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 4)); \
					  (vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 2)); \
					  (vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 1)); \
					  (m) = _mm_extract_epi16((vm), 0)

	uint8_t max = 0;		                     /* the max alignment score */
	int32_t end_read = 0;
	int32_t end_ref = 0; /* 1_based best alignment ending point; Initialized as isn't aligned - 0. */
	int32_t segLen = (readLen + 15) / 16; /* number of segment */
	
	/* array to record the largest score of each reference position */
	uint8_t* maxColumn = (uint8_t*) calloc(refLen, 1); 
	
	/* array to record the alignment read ending position of the largest score of each reference position */
	int32_t* end_read_column = (int32_t*) calloc(refLen, sizeof(int32_t));
	
	/* Define 16 byte 0 vector. */
	__m128i vZero = _mm_set1_epi32(0);

	__m128i* pvHStore = (__m128i*) calloc(segLen, sizeof(__m128i));
	__m128i* pvHLoad = (__m128i*) calloc(segLen, sizeof(__m128i));
	__m128i* pvE = (__m128i*) calloc(segLen, sizeof(__m128i));
	__m128i* pvHmax = (__m128i*) calloc(segLen, sizeof(__m128i));

	int32_t i, j, k;
	for (i = 0; LIKELY(i < segLen); i ++) {
		pvHStore[i] = vZero;
		pvE[i] = vZero;
	}
	
	/* 16 byte insertion begin vector */
	__m128i vInserB = _mm_set1_epi8(weight_insertB);
	
	/* 16 byte insertion extension vector */
	__m128i vInserE = _mm_set1_epi8(weight_insertE);	
	
	/* 16 byte deletion begin vector */
	__m128i vDeletB = _mm_set1_epi8(weight_deletB);	

	/* 16 byte deletion extension vector */
	__m128i vDeletE = _mm_set1_epi8(weight_deletE);	

	/* 16 byte bias vector */
	__m128i vBias = _mm_set1_epi8(bias);	

	__m128i vMaxScore = vZero; /* Trace the highest score of the whole SW matrix. */
	__m128i vMaxMark = vZero; /* Trace the highest score till the previous column. */	
	__m128i vTemp;
	int32_t edge, begin = 0, end = refLen, step = 1;

	/* outer loop to process the reference sequence */
	if (ref_dir == 1) {
		begin = refLen - 1;
		end = -1;
		step = -1;
	}
	for (i = begin; LIKELY(i != end); i += step) {
		int32_t cmp;
		__m128i e, vF = vZero; /* Initialize F value to 0. 
							   Any errors to vH values will be corrected in the Lazy_F loop. 
							 */
		__m128i vH = pvHStore[segLen - 1];
		vH = _mm_slli_si128 (vH, 1); /* Shift the 128-bit value in vH left by 1 byte. */
		
		/* Swap the 2 H buffers. */
		__m128i* pv = pvHLoad;
		
		__m128i vMaxColumn = vZero; /* vMaxColumn is used to record the max values of column i. */
		
		__m128i* vP = vProfile + ref[i] * segLen; /* Right part of the vProfile */
		pvHLoad = pvHStore;
		pvHStore = pv;
		
		/* inner loop to process the query sequence */
		for (j = 0; LIKELY(j < segLen); ++j) {

			vH = _mm_adds_epu8(vH, _mm_load_si128(vP + j));
			vH = _mm_subs_epu8(vH, vBias); /* vH will be always > 0 */

			/* Get max from vH, vE and vF. */
			e = _mm_load_si128(pvE + j);
			vH = _mm_max_epu8(vH, e);
			vH = _mm_max_epu8(vH, vF);
			
			/* Update highest score encountered this far. */
			vMaxColumn = _mm_max_epu8(vMaxColumn, vH);
			
			/* Save vH values. */
			_mm_store_si128(pvHStore + j, vH);

			/* Update vE value. */
			vH = _mm_subs_epu8(vH, vInserB); /* saturation arithmetic, result >= 0 */
			e = _mm_subs_epu8(e, vInserE);
			e = _mm_max_epu8(e, vH);
			_mm_store_si128(pvE + j, e);
			
			/* Update vF value. */
			vF = _mm_subs_epu8(vF, vDeletE);
			vF = _mm_max_epu8(vF, vH);
			
			/* Load the next vH. */
			vH = _mm_load_si128(pvHLoad + j);
		}

		/* Lazy_F loop: has been revised to disallow adjecent insertion and then deletion, so don't update E(i, j), learn from SWPS3 */
		for (k = 0; LIKELY(k < 16); ++k) {
			vF = _mm_slli_si128 (vF, 1);
			for (j = 0; LIKELY(j < segLen); ++j) {
				vH = _mm_load_si128(pvHStore + j);
				vH = _mm_max_epu8(vH, vF);
				_mm_store_si128(pvHStore + j, vH);
				vH = _mm_subs_epu8(vH, vDeletB);
				vF = _mm_subs_epu8(vF, vDeletE);
				if (UNLIKELY(! _mm_movemask_epi8(_mm_cmpgt_epi8(vF, vH)))) goto end;
			}
		}

end:		
		vMaxScore = _mm_max_epu8(vMaxScore, vMaxColumn);
		vTemp = _mm_cmpeq_epi8(vMaxMark, vMaxScore);
		cmp = _mm_movemask_epi8(vTemp);
		if (cmp != 0xffff) {
			uint8_t temp; 
			vMaxMark = vMaxScore;
			max16(temp, vMaxScore);
			vMaxScore = vMaxMark;
			
			if (LIKELY(temp > max)) {
				max = temp;
				if (max + bias >= 255) break;	//overflow
				end_ref = i;
			
				/* Store the column with the highest alignment score in order to trace the alignment ending position on read. */
				for (j = 0; LIKELY(j < segLen); ++j) // keep the H1 vector
					pvHmax[j] = pvHStore[j];
			}
		}
		
		/* Record the max score of current column. */	
		max16(maxColumn[i], vMaxColumn);
		if (maxColumn[i] == terminate) break;
	} 	

	/* Trace the alignment ending position on read. */
	uint8_t *t = (uint8_t*)pvHmax;
	int32_t column_len = segLen * 16;
	for (i = 0; LIKELY(i < column_len); ++i, ++t) {
		if (*t == max) {
			end_read = i / 16 + i % 16 * segLen;
			break;
		}
	}

	/* Find the most possible 2nd best alignment. */
	alignment_end* bests = (alignment_end*) calloc(2, sizeof(alignment_end));
	bests[0].score = max + bias >= 255 ? 255 : max;
	bests[0].ref = end_ref;
	bests[0].read = end_read;
	
	bests[1].score = 0;
	bests[1].ref = 0;
	bests[1].read = 0;

	edge = (end_ref - readLen / 2 - 1) > 0 ? (end_ref - readLen / 2 - 1) : 0;
	for (i = 0; i < edge; i ++) {
		if (maxColumn[i] > bests[1].score) 
			bests[1].score = maxColumn[i];
	}
	edge = (end_ref + readLen / 2 + 1) > refLen ? refLen : (end_ref + readLen / 2 + 1);
	for (i = edge; i < refLen; i ++) {
		if (maxColumn[i] > bests[1].score) 
			bests[1].score = maxColumn[i];
	}
	
	free(pvHStore);
	free(maxColumn);
	free(end_read_column);
	return bests;
}

__m128i* qP_word (int8_t* read_num,
				  int8_t* mat,
				  int32_t readLen,
				  int32_t n) { 
					
	int32_t
	segLen = (readLen + 7) / 8; 
	__m128i* vProfile = (__m128i*)calloc(n * segLen, sizeof(__m128i));
	int16_t* t = (int16_t*)vProfile;
	int32_t nt, i, j;
	int32_t segNum;
	
	/* Generate query profile rearrange query sequence & calculate the weight of match/mismatch */
	for (nt = 0; LIKELY(nt < n); nt ++) {
		for (i = 0; i < segLen; i ++) {
			j = i; 
			for (segNum = 0; LIKELY(segNum < 8) ; segNum ++) {
				*t++ = j>= readLen ? 0 : mat[nt * n + read_num[j]];
				j += segLen;
			}
		}
	}

	return vProfile;
}

alignment_end* sw_sse2_word (int8_t* ref, 
							 int8_t ref_dir,	// 0: forward ref; 1: reverse ref
							 int32_t refLen,
							 int32_t readLen, 
							 uint8_t weight_insertB, /* will be used as - */
							 uint8_t weight_insertE, /* will be used as - */
							 uint8_t weight_deletB,  /* will be used as - */
							 uint8_t weight_deletE,  /* will be used as - */
						     __m128i* vProfile,
							 uint16_t terminate) { 

#define max8(m, vm) (vm) = _mm_max_epi16((vm), _mm_srli_si128((vm), 8)); \
					(vm) = _mm_max_epi16((vm), _mm_srli_si128((vm), 4)); \
					(vm) = _mm_max_epi16((vm), _mm_srli_si128((vm), 2)); \
					(m) = _mm_extract_epi16((vm), 0)
	
	uint16_t max = 0;		                     /* the max alignment score */
	int32_t end_read = 0;
	int32_t end_ref = 0; /* 1_based best alignment ending point; Initialized as isn't aligned - 0. */
	int32_t segLen = (readLen + 7) / 8; /* number of segment */
	
	/* array to record the largest score of each reference position */
	uint16_t* maxColumn = (uint16_t*) calloc(refLen, 2); 
	
	/* array to record the alignment read ending position of the largest score of each reference position */
	int32_t* end_read_column = (int32_t*) calloc(refLen, sizeof(int32_t));
	
	/* Define 16 byte 0 vector. */
	__m128i vZero = _mm_set1_epi32(0);

	__m128i* pvHStore = (__m128i*) calloc(segLen, sizeof(__m128i));
	__m128i* pvHLoad = (__m128i*) calloc(segLen, sizeof(__m128i));
	__m128i* pvE = (__m128i*) calloc(segLen, sizeof(__m128i));
	__m128i* pvHmax = (__m128i*) calloc(segLen, sizeof(__m128i));

	int32_t i, j, k;
	for (i = 0; LIKELY(i < segLen); i ++) {
		pvHStore[i] = vZero;
		pvE[i] = vZero;
	}
	
	/* 16 byte insertion begin vector */
	__m128i vInserB = _mm_set1_epi16(weight_insertB);
	
	/* 16 byte insertion extension vector */
	__m128i vInserE = _mm_set1_epi16(weight_insertE);	
	
	/* 16 byte deletion begin vector */
	__m128i vDeletB = _mm_set1_epi16(weight_deletB);

	/* 16 byte deletion extension vector */
	__m128i vDeletE = _mm_set1_epi16(weight_deletE);	

	/* 16 byte bias vector */

	__m128i vMaxScore = vZero; /* Trace the highest score of the whole SW matrix. */
	__m128i vMaxMark = vZero; /* Trace the highest score till the previous column. */	
	__m128i vTemp;
	int32_t edge, begin = 0, end = refLen, step = 1;

	/* outer loop to process the reference sequence */
	if (ref_dir == 1) {
		begin = refLen - 1;
		end = -1;
		step = -1;
	}
	for (i = begin; LIKELY(i != end); i += step) {
		int32_t cmp;
		__m128i e, vF = vZero; /* Initialize F value to 0. 
							   Any errors to vH values will be corrected in the Lazy_F loop. 
							 */
		__m128i vH = pvHStore[segLen - 1];
		vH = _mm_slli_si128 (vH, 2); /* Shift the 128-bit value in vH left by 2 byte. */
		
		/* Swap the 2 H buffers. */
		__m128i* pv = pvHLoad;
		
		__m128i vMaxColumn = vZero; /* vMaxColumn is used to record the max values of column i. */
		
		__m128i* vP = vProfile + ref[i] * segLen; /* Right part of the vProfile */
		pvHLoad = pvHStore;
		pvHStore = pv;
		
		/* inner loop to process the query sequence */
		for (j = 0; LIKELY(j < segLen); j ++) {
			vH = _mm_adds_epi16(vH, _mm_load_si128(vP + j));

			/* Get max from vH, vE and vF. */
			e = _mm_load_si128(pvE + j);
			vH = _mm_max_epu8(vH, e);
			vH = _mm_max_epi16(vH, vF);
			vMaxColumn = _mm_max_epi16(vMaxColumn, vH);
			
			/* Save vH values. */
			_mm_store_si128(pvHStore + j, vH);

			/* Update vE value. */
			vH = _mm_subs_epu16(vH, vInserB); /* saturation arithmetic, result >= 0 */
			e = _mm_subs_epu16(e, vInserE);
			e = _mm_max_epi16(e, vH);
			_mm_store_si128(pvE + j, e);

			/* Update vF value. */
			vF = _mm_subs_epu16(vF, vDeletE);
			vF = _mm_max_epi16(vF, vH);
			
			/* Load the next vH. */
			vH = _mm_load_si128(pvHLoad + j);
		}

		/* Lazy_F loop: has been revised to disallow adjecent insertion and then deletion, so don't update E(i, j), learn from SWPS3 */
		for (k = 0; LIKELY(k < 8); ++k) {
			vF = _mm_slli_si128 (vF, 2);
			for (j = 0; LIKELY(j < segLen); ++j) {
				vH = _mm_load_si128(pvHStore + j);
				vH = _mm_max_epi16(vH, vF);
				_mm_store_si128(pvHStore + j, vH);
				vH = _mm_subs_epu16(vH, vDeletB);
				vF = _mm_subs_epu16(vF, vDeletE);
				if (UNLIKELY(! _mm_movemask_epi8(_mm_cmpgt_epi16(vF, vH)))) goto end;
			}
		}

end:	
		vMaxScore = _mm_max_epi16(vMaxScore, vMaxColumn);	
		vTemp = _mm_cmpeq_epi16(vMaxMark, vMaxScore);
		cmp = _mm_movemask_epi8(vTemp);
		if (cmp != 0xffff) {
			uint16_t temp; 
			vMaxMark = vMaxScore;
			max8(temp, vMaxScore);
			vMaxScore = vMaxMark;
			
			if (LIKELY(temp > max)) {
				max = temp;
				end_ref = i;
				for (j = 0; LIKELY(j < segLen); ++j) // keep the H1 vector
					pvHmax[j] = pvHStore[j];
			}
		}
		
		/* Record the max score of current column. */	
		max8(maxColumn[i], vMaxColumn);
		if (maxColumn[i] == terminate) break;
	} 	

	/* Trace the alignment ending position on read. */
	uint16_t *t = (uint16_t*)pvHmax;
	int32_t column_len = segLen * 8;
	for (i = 0; LIKELY(i < column_len); ++i, ++t) {
		if (*t == max) {
			end_read = i / 8 + i % 8 * segLen;
			break;
		}
	}

	/* Find the most possible 2nd best alignment. */
	alignment_end* bests = (alignment_end*) calloc(2, sizeof(alignment_end));
	bests[0].score = max;
	bests[0].ref = end_ref;
	bests[0].read = end_read;
	
	bests[1].score = 0;
	bests[1].ref = 0;
	bests[1].read = 0;

	edge = (end_ref - readLen / 2 - 1) > 0 ? (end_ref - readLen / 2 - 1) : 0;
	for (i = 0; i < edge; i ++) {
		if (maxColumn[i] > bests[1].score) 
			bests[1].score = maxColumn[i];
	}
	edge = (end_ref + readLen / 2 + 1) > refLen ? refLen : (end_ref + readLen / 2 + 1);
	for (i = edge; i < refLen; i ++) {
		if (maxColumn[i] > bests[1].score) 
			bests[1].score = maxColumn[i];
	}
	
	free(pvHStore);
	free(maxColumn);
	free(end_read_column);
	return bests;
}

// Convert a positive integer to a string.
char* itoa(int32_t i) {
	char* p0 = calloc(8, sizeof(char));
	char c[8], *p1 = c, *p = p0;
	do {
    	*p1 = '0' + (i % 10);
    	i /= 10;
		++p1;
    } while (i != 0);
	do {
		--p1;
		*p = *p1;
		++p;
	}while (p1 != c);
	*p = '\0';
    return p0;
}

char* banded_sw (int8_t* ref,
				 int8_t* read, 
				 int32_t refLen, 
				 int32_t readLen,
				 int32_t score,
				 uint32_t weight_insertB,  /* will be used as - */
				 uint32_t weight_insertE,  /* will be used as - */
				 uint32_t weight_deletB,   /* will be used as - */
				 uint32_t weight_deletE,   /* will be used as - */
				 int32_t band_width,
				 int8_t* mat,	/* pointer to the weight matrix */
				 int32_t n) {	

	char* cigar = (char*)calloc(16, sizeof(char)), *p = cigar, ci = 'M';
	char* cigar1, *p1;	// reverse cigar
	int32_t i, j, e, f, temp1, temp2, s = 16, c = 0, l, max = 0;
	int32_t width, width_d, *h_b, *e_b, *h_c;
	int8_t *direction, *direction_line;

	do {
		width = band_width * 2 + 3, width_d = band_width * 2 + 1;
		h_b = (int32_t*)calloc(width, sizeof(int32_t)); 
		e_b = (int32_t*)calloc(width, sizeof(int32_t)); 
		h_c = (int32_t*)calloc(width, sizeof(int32_t)); 

		direction = (int8_t*)calloc(width_d * readLen * 3, sizeof(int8_t));
		direction_line = direction;
		for (j = 1; LIKELY(j < width - 1); j ++) h_b[j] = 0;
		for (i = 0; LIKELY(i < readLen); i ++) {
			int32_t beg = 0, end = refLen - 1, u = 0, edge;
			j = i - band_width;	beg = beg > j ? beg : j; // band start
			j = i + band_width; end = end < j ? end : j; // band end
			edge = end + 1 < width - 1 ? end + 1 : width - 1;
			f = h_b[0] = e_b[0] = h_b[edge] = e_b[edge] = h_c[0] = 0;
			direction_line = direction + width_d * i * 3;

			for (j = beg; LIKELY(j <= end); j ++) {
				int32_t b, e1, f1, d, de, df, dh;
				set_u(u, band_width, i, j);	set_u(e, band_width, i - 1, j); 
				set_u(b, band_width, i, j - 1); set_u(d, band_width, i - 1, j - 1);
				set_d(de, band_width, i, j, 0);
				set_d(df, band_width, i, j, 1);
				set_d(dh, band_width, i, j, 2);

				temp1 = i == 0 ? -weight_insertB : h_b[e] - weight_insertB;
				temp2 = i == 0 ? -weight_insertE : e_b[e] - weight_insertE;
				e_b[u] = temp1 > temp2 ? temp1 : temp2;
				direction_line[de] = temp1 > temp2 ? 3 : 2;
		
				temp1 = h_c[b] - weight_deletB;
				temp2 = f - weight_deletE;
				f = temp1 > temp2 ? temp1 : temp2;
				direction_line[df] = temp1 > temp2 ? 5 : 4;
				
				e1 = e_b[u] > 0 ? e_b[u] : 0;
				f1 = f > 0 ? f : 0;
				temp1 = e1 > f1 ? e1 : f1;
				temp2 = h_b[d] + mat[ref[j] * n + read[i]];
				h_c[u] = temp1 > temp2 ? temp1 : temp2;
		
				if (h_c[u] > max) max = h_c[u];
		
				if (temp1 <= temp2) direction_line[dh] = 1;
				else direction_line[dh] = e1 > f1 ? direction_line[de] : direction_line[df];
			}
			for (j = 1; j <= u; j ++) h_b[j] = h_c[j];
		}
		band_width *= 2;
	} while (LIKELY(max < score));
	band_width /= 2;

	// trace back
	i = readLen - 1;
	j = refLen - 1;
	e = 0;	// Count the number of M, D or I.
	f = 'M';
	temp2 = 2;	// h
	while (LIKELY(i > 0)) {
		set_d(temp1, band_width, i, j, temp2);
		switch (direction_line[temp1]) {
			case 1: 
				--i;
				--j;
				temp2 = 2;
				direction_line -= width_d * 3;
				f = 'M';
				break;
			case 2:
			 	--i;
				temp2 = 0;	// e
				direction_line -= width_d * 3;
				f = 'I';
				break;		
			case 3:
				--i;
				temp2 = 2;
				direction_line -= width_d * 3;
				f = 'I';
				break;
			case 4:
				--j;
				temp2 = 1;
				f = 'D';
				break;
			case 5:
				--j;
				temp2 = 2;
				f = 'D';
				break;
			default: 
				fprintf(stderr, "Trace back error: %d.\n", direction_line[temp1 - 1]);
				return 0;
		}
		if (f == ci) ++ e;
		else {
			char* num = itoa(e);
			l = strlen(num);
			c += l + 1;
			if (c >= s) {
				++s;
				kroundup32(s);
				cigar = realloc(cigar, s * sizeof(char));
				p = cigar + c - l - 1;
			}
			strcpy(p, num);
			free(num);
			p += l;
			*p = ci;
			ci = f;
			++p;
			e = 1;
		}
	}
	if (f == 'M') {
		char* num = itoa(e + 1);
		l = strlen(num);
		c += l + 1;
		if (c >= s) {
			++s;
			kroundup32(s);
			cigar = realloc(cigar, s * sizeof(char));
			p = cigar + c - l - 1;
		}
		strcpy(p, num);
		free(num);
		p += l;
		*p = 'M';
	}else {
		char* num = itoa(e);
		l = strlen(num);
		c += l + 3;	
		if (c >= s) {
			++s;
			kroundup32(s);
			cigar = realloc(cigar, s * sizeof(char));
			p = cigar + c - l - 3;
		}
		strcpy(p, num);
		free(num);
		p += l;
		*p = f;
		++p;
		*p = '1';
		++p;
		*p = 'M';
	}
	++p; *p = '\0';

	// reverse cigar
	cigar1 = (char*)calloc(strlen(cigar) + 1, sizeof(char));
	p1 = cigar1;
	l = 0;
	ci = 'M';
	p = cigar + strlen(cigar) - 1;
	while (LIKELY(p >= cigar)) {
		if (*p == 'M' || *p == 'I' || *p == 'D') {
			if (l > 0) {
				strncpy(p1, p + 1, l);
				p1 += l;
				*p1 = ci;
				++p1;
			}
			ci = *p;
			--p;
			l = 0;
		} else {
			++l;
			--p;
		}
	}
	strncpy(p1, p + 1, l);
	p1 += l;
	*p1 = ci;
	++p1;
	*p1 = '\0';

	free(direction);
	free(h_c);
	free(e_b);
	free(h_b);
	free(cigar);
	return cigar1;
}

int8_t* seq_reverse(int8_t* seq, int32_t end)	/* end is 0-based alignment ending position */	
{									
	int8_t* reverse = (int8_t*)calloc(end + 1, sizeof(int8_t));	
	int32_t start = 0;
	while (LIKELY(start <= end)) {			
		reverse[start] = seq[end];		
		reverse[end] = seq[start];		
		++ start;					
		-- end;						
	}								
	return reverse;					
}
		
profile* ssw_init (init_param* init) {
	int32_t n = init->n;	
	profile* p = (profile*)calloc(1, sizeof(struct _profile));
	p->profile_byte = 0;
	p->profile_word = 0;
//	p->reverse_byte = 0;
//	p->reverse_word = 0;
	
	if (init->score_size ==	0 || init->score_size == 2) p->profile_byte = qP_byte (init->read, init->mat, init->readLen, n, 4);
	if (init->score_size ==	1 || init->score_size == 2) p->profile_word = qP_word (init->read, init->mat, init->readLen, n);
/*	if (init->rc_read) {
		if (init->score_size ==	0 || init->score_size == 2) p->reverse_byte = qP_byte (init->rc_read, init->mat, init->readLen, n, 4);
		if (init->score_size ==	1 || init->score_size == 2) p->reverse_word = qP_word (init->rc_read, init->mat, init->readLen, n);
	} */	
	p->read = init->read;
//	p->rc_read = init->rc_read;
	p->mat = init->mat;
	p->readLen = init->readLen;
	p->n = init->n;
	return p;
}

void init_destroy (profile* p) {
	free(p->profile_byte);
	free(p->profile_word);
//	free(p->reverse_byte);
//	free(p->reverse_word);
	free(p->read);
//	free(p->mat);
//	free(p->rc_read);
	free(p);
}

align* ssw_align (align_param* a) {
	alignment_end* bests = 0, *bests_reverse = 0;
	__m128i* vP = 0;
	int32_t readLen = a->prof->readLen, word = 0, refLen = 0;
	int32_t n = a->prof->n, band_width = 0;
	int8_t* read_reverse = 0;
	align* r = (align*)calloc(1, sizeof(align));
//	r->strand = 0;
	r->score1 = 0;
	r->score2 = 0;
	r->ref_begin1 = 0;
	r->ref_end1 = 0;
	r->read_begin1 = 0;
	r->read_end1 = 0;
	r->ref_end2 = 0;
	r->cigar = 0;

	// Find the alignment scores and ending positions
	if (a->prof->profile_byte) {
		bests = sw_sse2_byte(a->ref, 0, a->refLen, readLen, a->weight_insertB, a->weight_insertE, a->weight_deletB, a->weight_deletE, a->prof->profile_byte, -1, 4);
		if (a->prof->profile_word && bests[0].score == 225) {
			bests = sw_sse2_word(a->ref, 0, a->refLen, readLen, a->weight_insertB, a->weight_insertE, a->weight_deletB, a->weight_deletE, a->prof->profile_word, -1);
			word = 1;
		}
	}else if (a->prof->profile_word) {
		bests = sw_sse2_word(a->ref, 0, a->refLen, readLen, a->weight_insertB, a->weight_insertE, a->weight_deletB, a->weight_deletE, a->prof->profile_word, -1);
		word = 1;
	}else {
		fprintf(stderr, "The score_size variable of the init_param structure is not correctly assigned.\n");
		return 0;
	}
	r->score1 = bests[0].score;
	r->score2 = bests[1].score;
	r->ref_end1 = bests[0].ref + 1;
	r->read_end1 = bests[0].read + 1;
	r->ref_end2 = bests[1].ref + 1;
/*	if (a->prof->reverse_byte) {
		bests = sw_sse2_byte(a->ref, 0, a->refLen, readLen, a->weight_insertB, a->weight_insertE, a->weight_deletB, a->weight_deletE, a->prof->reverse_byte, -1, 4);
		if (a->prof->reverse_word && bests[0].score == 225) {
			bests = sw_sse2_word(a->ref, 0, a->refLen, readLen, a->weight_insertB, a->weight_insertE, a->weight_deletB, a->weight_deletE, a->prof->reverse_word, -1);
			rc_word = 1;
		}
	}else if (a->prof->reverse_word) {
		bests = sw_sse2_word(a->ref, 0, a->refLen, readLen, a->weight_insertB, a->weight_insertE, a->weight_deletB, a->weight_deletE, a->prof->reverse_word, -1);
		rc_word = 1;
	}
	if (bests[0].score > r->score1) {
		r->score1 = bests[0].score;
		r->score2 = bests[1].score;
		r->ref_end1 = bests[0].ref + 1;
		r->read_end1 = bests[0].read + 1;
		r->ref_end2 = bests[1].ref + 1;
		r->strand = 1;
		word = rc_word;
	}*/ 
	free(bests);
	if ((a->begin == 0 && a->align == 0) || r->score1 == 225) goto end;

	// Find the beginning position of the best alignment.
	read_reverse = seq_reverse(a->prof->read, r->read_end1 - 1);
//	else read_reverse = seq_reverse(a->prof->rc_read, r->read_end1 - 1);
	if (word == 0) {
		vP = qP_byte(read_reverse, a->prof->mat, r->read_end1, n, 4);
//	fprintf(stderr, "r->score1: %d\n", r->score1);
		bests_reverse = sw_sse2_byte(a->ref, 1, r->ref_end1, r->read_end1, a->weight_insertB, a->weight_insertE, a->weight_deletB, a->weight_deletE, vP, r->score1, 4);
	} else {
		vP = qP_word(read_reverse, a->prof->mat, r->read_end1, n);
		bests_reverse = sw_sse2_word(a->ref, 1, r->ref_end1, r->read_end1, a->weight_insertB, a->weight_insertE, a->weight_deletB, a->weight_deletE, vP, r->score1);
	}
	free(vP);
	free(read_reverse);
	r->ref_begin1 = bests_reverse[0].ref + 1;
	r->read_begin1 = r->read_end1 - bests_reverse[0].read;
	free(bests_reverse);
	if (a->align == 0) goto end;

	// Generate cigar.
	refLen = r->ref_end1 - r->ref_begin1 + 1;
	readLen = r->read_end1 - r->read_begin1 + 1;
	band_width = abs(refLen - readLen) + 1;
//	fprintf(stderr, "refLen: %d\treadLen: %d\tband_width: %d\n", refLen, readLen, band_width);
	r->cigar = banded_sw(a->ref + r->ref_begin1 - 1, a->prof->read + r->read_begin1 - 1, refLen, readLen, r->score1, a->weight_insertB, a->weight_insertE, a->weight_deletB, a->weight_deletE, band_width, a->prof->mat, n);
//	else r->cigar = banded_sw(a->ref + r->ref_begin1 - 1, a->prof->rc_read + r->read_begin1 - 1, refLen, readLen, r->score1, a->weight_insertB, a->weight_insertE, a->weight_deletB, a->weight_deletE, band_width, a->prof->mat, n);
	
end: 
	return r;
}

void align_destroy (align* c) {
	free(c->cigar);
	free(c);
}

