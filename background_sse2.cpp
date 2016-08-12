#include "background.h"
#define USE_SSE2
#include "sse_mathfun.h"

void Background::processFrameSIMD(InputArray _src, OutputArray _foregroundMask)
{
	Mat src = _src.getMat(), foregroundMask = _foregroundMask.getMat();
	uint32_t nPixels = src.size().area();

	for (uint32_t idx = 0; idx < nPixels; idx += 4)
	{
		uint32_t fgMask = processPixelSSE2(src.data + 3*idx,
										   (float*)gaussians + 5*GAUSSIANS_PER_PIXEL*idx,
										   currentBackground.data + 3*idx,
										   (float*)currentStdDev.data + idx);

		foregroundMask.at<uint32_t>(idx/4) = fgMask;
	}

	medianBlur(foregroundMask, foregroundMask, 3);
}


uint32_t Background::processPixelSSE2(const uint8_t* frame, float* gaussian,
									  uint8_t* currentBackground, float* currentStdDev)
{
	__m128i bgr = _mm_loadu_si128((const __m128i*)frame); 
	// bgr now contains:
	// B1G1R1 B2G2R2 B3G3R3 B4G4R4 B5G5R5 B6 
	// we're only interested in first four pixels.
	// they need to be converted to float and reordered into
	// B1B2B3B4 G1G2G3G4 R1R2R3R4
	
	bgr = _mm_slli_si128(bgr, 4);
	// shift bgr 4 bits left. now it contains:
	// 0000 B1G1R1B2 G2R2B3G3 R3B4G4R4

	// to convert uint8_t to floats it'd easy to use _mm_cvtpu8_ps, but it takes __m64 as argument
	// instead, we'll have to expand uint8_t to uint32_t and then use _mm_cvtepi32_ps
	__m128i hi16 = _mm_unpacklo_epi8(bgr, _mm_set1_epi8(0)); // 0G2 0R2 0B3 0G3 0R3 0B4 0G4 0R4
	__m128i lo16 = _mm_unpackhi_epi8(bgr, _mm_set1_epi8(0)); // 00  00  00  00  0B1 0G1 0R1 0B2
	// now we have 16-bit values, expand them to 32-bit
	__m128i lo32 = _mm_unpackhi_epi16(lo16, _mm_set1_epi16(0)); // 000R3 000B4 000G4 000R4
	__m128i mi32 = _mm_unpacklo_epi16(lo16, _mm_set1_epi16(0)); // 000G2 000R2 000B3 000G3
	__m128i hi32 = _mm_unpackhi_epi16(hi16, _mm_set1_epi16(0)); // 000B1 000G1 000R1 000B2

	// now convert to floats.
	// from now on, {B,G,R}n represent 32-bit floats, not 8-bit bytes.
	__m128 lo = _mm_cvtepi32_ps(lo32); // R3 B4 G4 R4
	__m128 mi = _mm_cvtepi32_ps(mi32); // G2 R2 B3 G3
	__m128 hi = _mm_cvtepi32_ps(hi32); // B1 G1 R1 B2

	hi = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(1, 2, 3, 0)); // B1 B2 R1 G1
	__m128 r1_r2 = _mm_shuffle_ps(hi, mi, _MM_SHUFFLE(1, 0, 0, 2)); // R1 B1 G2 R2

	__m128 hi_tmp = _mm_shuffle_ps(hi, mi, _MM_SHUFFLE(3, 2, 1, 0)); // B1 B2 B3 R2
	hi_tmp = _mm_shuffle_ps(hi_tmp, hi_tmp, _MM_SHUFFLE(0, 2, 1, 3)); // R2 B2 B3 B1
	hi_tmp = _mm_move_ss(hi_tmp, _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(0, 0, 0, 1))); // B4 B2 B3 B1
	hi_tmp = _mm_shuffle_ps(hi_tmp, hi_tmp, _MM_SHUFFLE(0, 2, 1, 3)); // B1 B2 B3 B4

	mi = _mm_shuffle_ps(mi, mi, _MM_SHUFFLE(1, 3, 0, 2)); // B3 G2 G3 R2
	mi = _mm_move_ss(mi, _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(0, 0, 0, 3))); // G1 G2 G3 R2
	mi = _mm_shuffle_ps(mi, mi, _MM_SHUFFLE(0, 2, 1, 3)); // R2 G2 G3 G1
	mi = _mm_move_ss(mi, _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(0, 0, 0, 2))); // G4 G2 G3 G1
	mi = _mm_shuffle_ps(mi, mi, _MM_SHUFFLE(0, 2, 1, 3)); // G1 G2 G3 G4

	lo = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(3, 0, 1, 2)); // G4 B4 R3 R4 
	lo = _mm_shuffle_ps(r1_r2, lo, _MM_SHUFFLE(3, 2, 3, 0)); // R1 R2 R3 R4

	// colours are now nicely shuffled
	__m128 B = hi_tmp;
	__m128 G = mi;
	__m128 R = lo;
	
	// memory layout looks like this:
	// (gaussian + 0): meanB for Gaussian #1
	// (gaussian + 4): meanB for Gaussian #2
	// (gaussian + 8): meanB for Gaussian #3
	// (gaussian + 12): meanG for Gaussian #1
	// (gaussian + 16): meanG for Gaussian #2
	// (gaussian + 20): meanG for Gaussian #3
	// and so on...
	
	__m128 matched  = _mm_setzero_ps();
	__m128 weightSum = _mm_setzero_ps();
	for (int i = 0; i < GAUSSIANS_PER_PIXEL; i++)
	{
		int offset = 4 * i;
		
		__m128 meanB	= _mm_load_ps(00 + offset + gaussian); // B1 B2 B3 B4 
		__m128 meanG	= _mm_load_ps(12 + offset + gaussian); // G1 G2 G3 G4 
		__m128 meanR	= _mm_load_ps(24 + offset + gaussian); // R1 R2 R3 R4 
		__m128 variance = _mm_load_ps(36 + offset + gaussian); // V1 V2 V3 V4 
		__m128 weight	= _mm_load_ps(48 + offset + gaussian); // W1 W2 W3 W4

		// dX = meanX - X
		__m128 dB = _mm_sub_ps(meanB, B);
		__m128 dG = _mm_sub_ps(meanG, G);
		__m128 dR = _mm_sub_ps(meanR, R);

		// dX = dX^2
		dB = _mm_mul_ps(dB, dB);

		dG = _mm_mul_ps(dG, dG);
		dR = _mm_mul_ps(dR, dR);

		// distance = dB + dG + dR
		// bear in mind that in this context dX is already squared
		__m128 distance = _mm_add_ps(_mm_add_ps(dB, dG), dR);
		__m128 distanceSqrt = _mm_sqrt_ps(distance);

		__m128 stdDev = _mm_sqrt_ps(variance);

		//if (sqrt(distance) < 2.5*sqrt(gauss.variance))
		__m128 mask = _mm_cmplt_ps(distanceSqrt, _mm_mul_ps(stdDev, _mm_set1_ps(2.5)));
		// if (!matched)
		mask = _mm_andnot_ps(matched, mask);

		// mask now contains info if input pixels got matched with current Gaussian
		// if it happens so, mark it in 'matched'
		matched = _mm_or_ps(matched, mask);

		// calculate exponent
		__m128 exponent = _mm_mul_ps(distance, _mm_set1_ps(-0.5));
		exponent = _mm_div_ps(exponent, variance);

		__m128 eta = exp_ps(exponent);
		__m128 denominator = _mm_set1_ps(etaConst);
		denominator = _mm_mul_ps(denominator, stdDev);
		denominator = _mm_mul_ps(denominator, stdDev);
		denominator = _mm_mul_ps(denominator, stdDev);
		eta = _mm_div_ps(eta, denominator);

		__m128 rho = _mm_mul_ps(eta, _mm_set1_ps(learningRate));
		__m128 oneMinusRho = _mm_sub_ps(_mm_set1_ps(1.0), rho);

		__m128 newMeanB = _mm_mul_ps(oneMinusRho, meanB);
		newMeanB = _mm_add_ps(newMeanB, _mm_mul_ps(rho, B));
		meanB = _mm_or_ps(_mm_and_ps(mask, newMeanB), _mm_andnot_ps(mask, meanB));
		
		__m128 newMeanG = _mm_mul_ps(oneMinusRho, meanG);
		newMeanG = _mm_add_ps(newMeanG, _mm_mul_ps(rho, G));
		meanG = _mm_or_ps(_mm_and_ps(mask, newMeanG), _mm_andnot_ps(mask, meanG));

		__m128 newMeanR = _mm_mul_ps(oneMinusRho, meanR);
		newMeanR = _mm_add_ps(newMeanR, _mm_mul_ps(rho, R));
		meanR = _mm_or_ps(_mm_and_ps(mask, newMeanR), _mm_andnot_ps(mask, meanR));

		__m128 newVariance = _mm_mul_ps(oneMinusRho, variance);
		newVariance = _mm_add_ps(newVariance, _mm_mul_ps(rho, distance));
		variance = _mm_or_ps(_mm_and_ps(mask, newVariance), _mm_andnot_ps(mask, variance));

		// at this point, local copies of mean{B,G,R} and variance are updated.
		// weights are updated for Gaussians that didn't match, so let's invert mask first
		__m128i junk; 
		mask = _mm_xor_ps(mask, (__m128)_mm_cmpeq_epi8(junk,junk));	
		__m128 newWeight = _mm_mul_ps(weight, _mm_set1_ps(1.0 - learningRate));
		weight = _mm_or_ps(_mm_and_ps(mask, newWeight), _mm_andnot_ps(mask, weight));
		weightSum = _mm_add_ps(weightSum, weight);

		_mm_store_ps(00 + offset + gaussian, meanB);
		_mm_store_ps(12 + offset + gaussian, meanG);
		_mm_store_ps(24 + offset + gaussian, meanR);
		_mm_store_ps(36 + offset + gaussian, variance);
		_mm_store_ps(48 + offset + gaussian, weight);
	}

	// handle case when input data didn't match any of the Gaussians
	// we just update least probable Gaussian
	
	// first of all, load weights
	__m128 weights[3];
	// weights[0..3] will contains weights laid out like this:
	// weights[0]: weights of Gaussian #1 for adjacent pixels W1 W2 W3 W4
	// weights[1]: weights of Gaussian #2 for adjacent pixels W1 W2 W3 W4
	// weights[2]: weights of Gaussian #3 for adjacent pixels W1 W2 W3 W4
	weights[0] = _mm_load_ps(48 + 0 + gaussian);
	weights[1] = _mm_load_ps(48 + 4 + gaussian);
	weights[2] = _mm_load_ps(48 + 8 + gaussian);

	// find minimal weight
	__m128 minWeight = _mm_min_ps(weights[0], _mm_min_ps(weights[1], weights[2]));
	__m128i junk; 
	__m128 notMatched = _mm_xor_ps(matched, (__m128)_mm_cmpeq_epi8(junk,junk));	

	__m128 isMin, value;
	for (int i = 0; i < 3; i++)
	{
		isMin = _mm_cmpeq_ps(minWeight, weights[i]);
		// finding out where max value lies is not enough, we need to make sure we don't overwrite
		// values of Gaussians that were previously matched with input data.
		// so, AND isMin with inverted 'matched' mask
		isMin = _mm_and_ps(isMin, notMatched);

		// meanB
		value = _mm_load_ps(00 + 4*i + gaussian);
		value = _mm_or_ps(_mm_and_ps(isMin, B), _mm_andnot_ps(isMin, value));
		_mm_store_ps(00 + 4*i + gaussian, value);
		// meanG
		value = _mm_load_ps(12 + 4*i + gaussian);
		value = _mm_or_ps(_mm_and_ps(isMin, G), _mm_andnot_ps(isMin, value));
		_mm_store_ps(12 + 4*i + gaussian, value);
		// meanR
		value = _mm_load_ps(24 + 4*i + gaussian);
		value = _mm_or_ps(_mm_and_ps(isMin, R), _mm_andnot_ps(isMin, value));
		_mm_store_ps(24 + 4*i + gaussian, value);
		// variance
		value = _mm_load_ps(36 + 4*i + gaussian);
		value = _mm_or_ps(_mm_and_ps(isMin, _mm_set1_ps(initialVariance)), _mm_andnot_ps(isMin, value));
		_mm_store_ps(36 + 4*i + gaussian, value);

		// modify weights only locally, we'll need them in just a bit
		weights[i] = _mm_or_ps(_mm_and_ps(isMin, _mm_set1_ps(initialWeight)), _mm_andnot_ps(isMin, weights[i]));
	}

	// now we should make sure that sum of weights equals to 1.
	weightSum = _mm_add_ps(weights[0], _mm_add_ps(weights[1], weights[2]));
	for (int i = 0; i < 3; i++)
	{
		weights[i] = _mm_div_ps(weights[i], weightSum);
		_mm_store_ps(48 + 4*i + gaussian, weights[i]);
	}

	// finally, we're done with updating Gaussians.
	// now need to find most probable Gaussian and estimate if input pixels belong to foreground or not
	__m128 maxWeight = _mm_max_ps(weights[0], _mm_max_ps(weights[1], weights[2]));
	__m128 isMax;
	__m128 fgMask = _mm_setzero_ps();
	// save most probable values to update current background image and current stdDev image
	__m128 bgB, bgG, bgR, bgVariance; 

	for (int i = 0; i < 3; i++)
	{
		isMax = _mm_cmpeq_ps(maxWeight, weights[i]);

		__m128 meanB	= _mm_load_ps(00 + 4*i + gaussian);
		__m128 meanG	= _mm_load_ps(12 + 4*i + gaussian);
		__m128 meanR	= _mm_load_ps(24 + 4*i + gaussian);
		__m128 variance = _mm_load_ps(36 + 4*i + gaussian);
		
		__m128 newEpsilon_bg = _mm_set1_ps(2 * log(2 * M_PI));

		__m128 varianceTmp = log_ps(_mm_sqrt_ps(variance));
		varianceTmp = _mm_mul_ps(varianceTmp, _mm_set1_ps(3.0));
		newEpsilon_bg = _mm_add_ps(newEpsilon_bg, varianceTmp);

		__m128 dB = _mm_sub_ps(B, meanB);
		dB = _mm_mul_ps(_mm_set1_ps(0.5), _mm_mul_ps(dB, dB));
		newEpsilon_bg = _mm_add_ps(newEpsilon_bg, _mm_div_ps(dB, variance));

		__m128 dG = _mm_sub_ps(G, meanG);
		dG = _mm_mul_ps(_mm_set1_ps(0.5), _mm_mul_ps(dG, dG));
		newEpsilon_bg = _mm_add_ps(newEpsilon_bg, _mm_div_ps(dG, variance));

		__m128 dR = _mm_sub_ps(R, meanR);
		dR = _mm_mul_ps(_mm_set1_ps(0.5), _mm_mul_ps(dR, dR));
		newEpsilon_bg = _mm_add_ps(newEpsilon_bg, _mm_div_ps(dR, variance));

		__m128 newFgMask = _mm_cmpgt_ps(newEpsilon_bg, _mm_set1_ps(foregroundThreshold));
		fgMask = _mm_or_ps(_mm_and_ps(isMax, newFgMask), _mm_andnot_ps(isMax, fgMask));
		
		bgB = _mm_or_ps(_mm_and_ps(isMax, meanB), _mm_andnot_ps(isMax, bgB));
		bgG = _mm_or_ps(_mm_and_ps(isMax, meanG), _mm_andnot_ps(isMax, bgG));
		bgR = _mm_or_ps(_mm_and_ps(isMax, meanR), _mm_andnot_ps(isMax, bgR));
		bgVariance = _mm_or_ps(_mm_and_ps(isMax, variance), _mm_andnot_ps(isMax, bgVariance));
	}

	// update background image
	// what do we have so far?
	// bgB: B1 B2 B3 B4
	// bgG: G1 G2 G3 G4
	// bgR: R1 R2 R4 R4
	__m128 bgT = _mm_setzero_ps();
	_MM_TRANSPOSE4_PS(bgB, bgG, bgR, bgT);

	// convert to 32-bit ints
	__m128i bgBi = _mm_cvtps_epi32(bgB); // B1 G1 R1 00
	__m128i bgGi = _mm_cvtps_epi32(bgG); // B2 G2 R2 00
	__m128i bgRi = _mm_cvtps_epi32(bgR); // B3 G3 R3 00
	__m128i bgTi = _mm_cvtps_epi32(bgT); // B4 G4 R4 00
	// convert to 16-bit ints
	bgBi = _mm_packs_epi32(bgBi, bgGi); // B1 G1 R1 00 B2 G2 R2 00
	bgRi = _mm_packs_epi32(bgRi, bgTi); // B3 G3 R3 00 B4 G4 R4 00
	// convert to 8-bit ints
	bgBi = _mm_packs_epi16(bgBi, bgRi); // B1 G1 R1 00 B2 G2 R2 00 B3 G3 R3 00 B4 G4 R4 00

	// extract isolated triplets 
	__m128i b2g2r2 = _mm_and_si128(bgBi, _mm_setr_epi8(0,0,0,0,0xFF,0xFF,0xFF,0,0,0,0,0,0,0,0,0));
	__m128i b3g3r3 = _mm_and_si128(bgBi, _mm_setr_epi8(0,0,0,0,0,0,0,0,0xFF,0xFF,0xFF,0,0,0,0,0));
	__m128i b4g4r4 = _mm_and_si128(bgBi, _mm_setr_epi8(0,0,0,0,0,0,0,0,0,0,0,0,0xFF,0xFF,0xFF,0));
	// remove extracted bytes from bgBi 
	bgBi = _mm_andnot_si128(b2g2r2, bgBi);
	bgBi = _mm_andnot_si128(b3g3r3, bgBi);
	bgBi = _mm_andnot_si128(b4g4r4, bgBi);
	// shift extracted bytes
	b2g2r2 = _mm_srli_si128(b2g2r2, 1);
	b3g3r3 = _mm_srli_si128(b3g3r3, 2);
	b4g4r4 = _mm_srli_si128(b4g4r4, 3);
	// merge 
	bgBi = _mm_or_si128(_mm_or_si128(bgBi, b2g2r2), _mm_or_si128(b3g3r3, b4g4r4));

	// we can only write out 16 bytes at a time, but we only have 12 bytes to write
	__m128i bg = _mm_loadu_si128((__m128i*)currentBackground);
	__m128i bgMask = _mm_setr_epi32(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0);
	bg = _mm_or_si128(_mm_and_si128(bgMask, bgBi), _mm_andnot_si128(bgMask, bg));
	_mm_storeu_si128((__m128i*)currentBackground, bg);

	// save stdDev
	_mm_store_ps(currentStdDev, _mm_sqrt_ps(bgVariance));
	
	// return foreground mask
	uint8_t moveMask = _mm_movemask_ps(fgMask);
	uint32_t outMask = 0;
	outMask |= (moveMask & 0b00000001) << 0;
	outMask |= (moveMask & 0b00000010) << 8;
	outMask |= (moveMask & 0b00000100) << 16;
	outMask |= (moveMask & 0b00001000) << 24;

	return outMask;
}
