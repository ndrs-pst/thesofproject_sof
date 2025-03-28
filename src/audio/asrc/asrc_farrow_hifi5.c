// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2012-2025 Intel Corporation.

#include <sof/common.h>

#if SOF_USE_MIN_HIFI(5, ASRC)

#include "asrc_farrow.h"
#include <xtensa/tie/xt_hifi5.h>

LOG_MODULE_DECLARE(asrc, CONFIG_SOF_LOG_LEVEL);

void asrc_fir_filter16(struct asrc_farrow *src_obj, int16_t **output_buffers,
		       int index_output_frame)
{
	ae_valignx2 align_filter;
	ae_valign align_buffer;
	ae_int64 prod;
	ae_f32x2 filter01;
	ae_f32x2 filter23;
	ae_f16x4 buffer0123;
	ae_int32x4 *filter_p;
	ae_f16x4 *buffer_p;
	int n_limit;
	int ch;
	int n;
	int i;

	/*
	 *  Four filter bins are accumulated per iteration.
	 * 'n_limit' is therefore stored to reduce redundant
	 * calculations. Also handle possible interleaved output.
	 */
	n_limit = src_obj->filter_length >> 2;
	if (src_obj->output_format == ASRC_IOF_INTERLEAVED)
		i = src_obj->num_channels * index_output_frame;
	else
		i = index_output_frame;

	/* Iterate over each channel */
	for (ch = 0; ch < src_obj->num_channels; ch++) {
		/* Pointer to the beginning of the impulse response */
		filter_p = (ae_int32x4 *)&src_obj->impulse_response[0];

		/* Pointer to the buffered input data */
		buffer_p =
			(ae_f16x4 *)&src_obj->ring_buffers16[ch]
			[src_obj->buffer_write_position];

		/* Allows unaligned load of 64 bit per cycle */
		align_filter = AE_LA128_PP(filter_p);
		align_buffer = AE_LA64_PP(buffer_p);

		/* Initialise the accumulator */
		prod = AE_ZERO64();

		/* Iterate over the filter bins */
		for (n = 0; n < n_limit; n++) {
			/* Read four buffered samples at once */
			AE_LA16X4_IP(buffer0123, align_buffer, buffer_p);

			/* Store four bins of the impulse response */
			AE_LA32X2X2_IP(filter01, filter23, align_filter, filter_p);

			/* Multiply and accumulate */
			AE_MULAAAAFQ32X16(prod, filter01, filter23, buffer0123);
		}

		/* Shift with saturation, use filter01 as scratch */
		filter01 = AE_SLAI32S(AE_ROUND32F48SASYM(prod), 1);

		/* Round 'prod' to 16 bit and store it in
		 * (de-)interleaved format in the output buffers
		 */
		AE_S16_0_X(AE_ROUND16X4F32SSYM(filter01, filter01),
			   (ae_f16 *)&output_buffers[ch][i], 0);
	}
}

void asrc_fir_filter32(struct asrc_farrow *src_obj, int32_t **output_buffers,
		       int index_output_frame)
{
	ae_valignx2 align_filter;
	ae_valignx2 align_buffer;
	ae_f64 prod0;
	ae_f64 prod1;
	ae_f32x2 buffer23;
	ae_f32x2 buffer01;
	ae_f32x2 filter01;
	ae_f32x2 filter23;
	const ae_int32x4 *filter_p;
	const ae_int32x4 *buffer_p;
	int n_limit;
	int ch;
	int n;
	int i;

	/*
	 * Two filter bins are accumulated per iteration.
	 * 'n_limit' is therefore stored to reduce redundant
	 * calculations. Also handle possible interleaved output.
	 */
	n_limit = src_obj->filter_length >> 2;
	if (src_obj->output_format == ASRC_IOF_INTERLEAVED)
		i = src_obj->num_channels * index_output_frame;
	else
		i = index_output_frame;

	/* Iterate over each channel */
	for (ch = 0; ch < src_obj->num_channels; ch++) {
		/* Pointer to the beginning of the impulse response */
		filter_p = (ae_int32x4 *)&src_obj->impulse_response[0];

		/* Pointer to the buffered input data */
		buffer_p =
			(ae_int32x4 *)&src_obj->ring_buffers32[ch]
			[src_obj->buffer_write_position];

		/* Allows unaligned load of 64 bit per cycle */
		align_filter = AE_LA128_PP(filter_p);
		align_buffer = AE_LA128_PP(buffer_p);

		/* Initialise the accumulators */
		prod0 = AE_ZERO64();
		prod1 = AE_ZERO64();

		/* Iterate over the filter bins */
		for (n = 0; n < n_limit; n++) {
			/* Read four buffered samples */
			AE_LA32X2X2_IP(buffer01, buffer23, align_buffer, buffer_p);

			/* Load four coefficients of the impulse response */
			AE_LA32X2X2_IP(filter01, filter23, align_filter, filter_p);

			/* Multiply and accumulate */
			AE_MULAAF2D32RA_HH_LL(prod0, prod1, buffer01, buffer23, filter01, filter23);
		}

		/* Add up the two accumulators */
		prod0 = AE_ADD64S(prod0, prod1);

		/* Shift with saturation */
		buffer01 = AE_SLAI32S(AE_ROUND32F48SASYM(prod0), 1);

		/* Store 'buffer01' in (de-)interleaved format in the output buffers */
		AE_S32_L_X(buffer01, (ae_f32 *)&output_buffers[ch][i], 0);
	}
}

/* + ALGORITHM SPECIFIC FUNCTIONS */

void asrc_calc_impulse_response_n4(struct asrc_farrow *src_obj)
{
	ae_f32x2 time_x2;
	ae_f32x2 a10;
	ae_f32x2 a32;
	ae_f32x2 b10;
	ae_f32x2 b32;
	ae_int32x4 *filter_P;
	ae_int32x4 *result_P;
	ae_valignx2 align_f;
	ae_valignx2 align_out;
	int i;
	int n;

	/* Set the pointer tot the polyphase filters */
	filter_P = (ae_int32x4 *)&src_obj->polyphase_filters[0];

	/*
	 * Set the pointer to the impulse response.
	 * This is where the result is stored.
	 */
	result_P = (ae_int32x4 *)&src_obj->impulse_response[0];

	/* allow unaligned load of 64 bit of polyphase filter coefficients */
	align_f = AE_LA128_PP(filter_P);
	align_out = AE_ZALIGN128();

	/* Get the current fractional time */
	time_x2 = AE_L32_X((ae_f32 *)&src_obj->time_value, 0);
	time_x2 = AE_SLAI32S(time_x2, 4);

	/*
	 * Generates two impulse response bins per iterations.
	 * 'index_limit' is therefore stored to reduce redundant
	 * calculations.
	 */
	n = src_obj->filter_length >> 2;
	for (i = 0; i < n; i++) {
		/*
		 * The polyphase filters lie in storage as follows
		 * (For N = 4, M = 64):
		 * [g3,0][g3,1][g3,2][g3,3][g2,0][g2,1][g2,2][g2,3]...
		 * [g0,0][g0,1][g0,2][g0,3][g3,4][g3,5][g3,6][g3,7]...
		 * [g0,60][g0,61][g0,62][g0,63]
		 *
		 * Since the polyphase filter coefficients are stored
		 * in an appropriate order, we can just load them up,
		 * one after another.
		 */

		/* Load coefficients g3,4*i g3,4*i+1 g3,4*i+2, g3,4*i+3 */
		AE_LA32X2X2_IP(b10, b32, align_f, filter_P);

		/* Load coefficients g2,4*i g2,4*i+1 g2,4*i+2, g2,4*i+3 */
		AE_LA32X2X2_IP(a10, a32, align_f, filter_P);

		/*
		 * Use the 'Horner's Method' to calculate the result
		 * in a numerically stable and efficient way:
		 *
		 * Example for one coefficient (N = 4):
		 * g_out,m = ((g3,m*t + g2,m)*t + g1,m)*t + g0,m
		 */
		AE_MULAFP32X2RS(a10, b10, time_x2);	/* Dual-MAC, could not find */
		AE_MULAFP32X2RS(a32, b32, time_x2);	/* suitable HiFi5 quad-MAC */

		/* Load coefficients g1,4*i g1,4*i+1 g1,4*i+2, g1,4*i+3 */
		AE_LA32X2X2_IP(b10, b32, align_f, filter_P);
		AE_MULAFP32X2RS(b10, a10, time_x2);
		AE_MULAFP32X2RS(b32, a32, time_x2);

		/* Load coefficients g0,4*i g0,4*i+1 g0,4*i+2, g0,4*i+3 */
		AE_LA32X2X2_IP(a10, a32, align_f, filter_P);
		AE_MULAFP32X2RS(a10, b10, time_x2);
		AE_MULAFP32X2RS(a32, b32, time_x2);

		/* Store the result */
		AE_SA32X2X2_IP(a10, a32, align_out, result_P);
	}

	AE_SA128POS_FP(align_out, result_P);
}

void asrc_calc_impulse_response_n5(struct asrc_farrow *src_obj)
{
	/*
	 * See 'calc_impulse_response_n4' for a detailed description
	 * of the algorithm and data handling
	 */
	ae_f32x2 time_x2;
	ae_f32x2 a10;
	ae_f32x2 a32;
	ae_f32x2 b10;
	ae_f32x2 b32;
	ae_int32x4 *filter_P;
	ae_int32x4 *result_P;
	ae_valignx2 align_f;
	ae_valignx2 align_out;
	int i;
	int n;

	filter_P = (ae_int32x4 *)&src_obj->polyphase_filters[0];
	result_P = (ae_int32x4 *)&src_obj->impulse_response[0];

	align_f = AE_LA128_PP(filter_P);
	align_out = AE_ZALIGN128();

	time_x2 = AE_L32_X((ae_f32 *)&src_obj->time_value, 0);
	time_x2 = AE_SLAI32S(time_x2, 4);

	n = src_obj->filter_length >> 2;
	for (i = 0; i < n; i++) {
		AE_LA32X2X2_IP(b10, b32, align_f, filter_P);
		AE_LA32X2X2_IP(a10, a32, align_f, filter_P);
		AE_MULAFP32X2RS(a10, b10, time_x2);
		AE_MULAFP32X2RS(a32, b32, time_x2);

		AE_LA32X2X2_IP(b10, b32, align_f, filter_P);
		AE_MULAFP32X2RS(b10, a10, time_x2);
		AE_MULAFP32X2RS(b32, a32, time_x2);

		AE_LA32X2X2_IP(a10, a32, align_f, filter_P);
		AE_MULAFP32X2RS(a10, b10, time_x2);
		AE_MULAFP32X2RS(a32, b32, time_x2);

		AE_LA32X2X2_IP(b10, b32, align_f, filter_P);
		AE_MULAFP32X2RS(b10, a10, time_x2);
		AE_MULAFP32X2RS(b32, a32, time_x2);

		AE_SA32X2X2_IP(b10, b32, align_out, result_P);

	}
	AE_SA128POS_FP(align_out, result_P);
}

void asrc_calc_impulse_response_n6(struct asrc_farrow *src_obj)
{
	/*
	 * See 'calc_impulse_response_n4' for a detailed description
	 * of the algorithm and data handling
	 */
	ae_f32x2 time_x2;
	ae_f32x2 a10;
	ae_f32x2 a32;
	ae_f32x2 b10;
	ae_f32x2 b32;
	ae_int32x4 *filter_P;
	ae_int32x4 *result_P;
	ae_valignx2 align_f;
	ae_valignx2 align_out;
	int i;
	int n;

	filter_P = (ae_int32x4 *)&src_obj->polyphase_filters[0];
	result_P = (ae_int32x4 *)&src_obj->impulse_response[0];

	align_f = AE_LA128_PP(filter_P);
	align_out = AE_ZALIGN128();

	time_x2 = AE_L32_X((ae_f32 *)&src_obj->time_value, 0);
	time_x2 = AE_SLAI32S(time_x2, 4);

	n = src_obj->filter_length >> 2;
	for (i = 0; i < n; i++) {
		AE_LA32X2X2_IP(b10, b32, align_f, filter_P);
		AE_LA32X2X2_IP(a10, a32, align_f, filter_P);
		AE_MULAFP32X2RS(a10, b10, time_x2);
		AE_MULAFP32X2RS(a32, b32, time_x2);

		AE_LA32X2X2_IP(b10, b32, align_f, filter_P);
		AE_MULAFP32X2RS(b10, a10, time_x2);
		AE_MULAFP32X2RS(b32, a32, time_x2);

		AE_LA32X2X2_IP(a10, a32, align_f, filter_P);
		AE_MULAFP32X2RS(a10, b10, time_x2);
		AE_MULAFP32X2RS(a32, b32, time_x2);

		AE_LA32X2X2_IP(b10, b32, align_f, filter_P);
		AE_MULAFP32X2RS(b10, a10, time_x2);
		AE_MULAFP32X2RS(b32, a32, time_x2);

		AE_LA32X2X2_IP(a10, a32, align_f, filter_P);
		AE_MULAFP32X2RS(a10, b10, time_x2);
		AE_MULAFP32X2RS(a32, b32, time_x2);

		AE_SA32X2X2_IP(a10, a32, align_out, result_P);

	}
	AE_SA128POS_FP(align_out, result_P);
}

void asrc_calc_impulse_response_n7(struct asrc_farrow *src_obj)
{
	/*
	 * See 'calc_impulse_response_n4' for a detailed description
	 * of the algorithm and data handling
	 */
	ae_f32x2 time_x2;
	ae_f32x2 a10;
	ae_f32x2 a32;
	ae_f32x2 b10;
	ae_f32x2 b32;
	ae_int32x4 *filter_P;
	ae_int32x4 *result_P;
	ae_valignx2 align_f;
	ae_valignx2 align_out;
	int i;
	int n;

	filter_P = (ae_int32x4 *)&src_obj->polyphase_filters[0];
	result_P = (ae_int32x4 *)&src_obj->impulse_response[0];

	align_f = AE_LA128_PP(filter_P);
	align_out = AE_ZALIGN128();

	time_x2 = AE_L32_X((ae_f32 *)&src_obj->time_value, 0);
	time_x2 = AE_SLAI32S(time_x2, 4);

	n = src_obj->filter_length >> 2;
	for (i = 0; i < n; i++) {
		AE_LA32X2X2_IP(b10, b32, align_f, filter_P);
		AE_LA32X2X2_IP(a10, a32, align_f, filter_P);
		AE_MULAFP32X2RS(a10, b10, time_x2);
		AE_MULAFP32X2RS(a32, b32, time_x2);

		AE_LA32X2X2_IP(b10, b32, align_f, filter_P);
		AE_MULAFP32X2RS(b10, a10, time_x2);
		AE_MULAFP32X2RS(b32, a32, time_x2);

		AE_LA32X2X2_IP(a10, a32, align_f, filter_P);
		AE_MULAFP32X2RS(a10, b10, time_x2);
		AE_MULAFP32X2RS(a32, b32, time_x2);

		AE_LA32X2X2_IP(b10, b32, align_f, filter_P);
		AE_MULAFP32X2RS(b10, a10, time_x2);
		AE_MULAFP32X2RS(b32, a32, time_x2);

		AE_LA32X2X2_IP(a10, a32, align_f, filter_P);
		AE_MULAFP32X2RS(a10, b10, time_x2);
		AE_MULAFP32X2RS(a32, b32, time_x2);

		AE_LA32X2X2_IP(b10, b32, align_f, filter_P);
		AE_MULAFP32X2RS(b10, a10, time_x2);
		AE_MULAFP32X2RS(b32, a32, time_x2);

		AE_SA32X2X2_IP(b10, b32, align_out, result_P);
	}
	AE_SA128POS_FP(align_out, result_P);
}

#endif /* ASRC_HIFI_5 */
