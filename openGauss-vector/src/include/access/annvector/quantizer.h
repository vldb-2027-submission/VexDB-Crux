#ifndef QUANTIZER_H
#define QUANTIZER_H

#include "access/annvector/floatvector.h"

enum class QuantizerType : uint8
{
	NONE = 0,
	PQ,
	RABITQ
	/* others... */
};


extern void validate_quantizer(const char *value);
extern QuantizerType extract_qt(const char *value);

#endif