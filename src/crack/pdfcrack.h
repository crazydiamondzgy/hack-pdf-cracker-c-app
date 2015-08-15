#ifndef _PDFCRACK_H_
#define _PDFCRACK_H_

#include <stdio.h>
#include "common.h"

#ifdef __cplusplus
extern "C"
{
#endif

void init_pdf(void * pImageFeather, int iElementCountFeather, int iElementSizeFeather);
void *crack_pdf(char *string, unsigned int len);

#endif /** _PDFCRACK_H_ */

#ifdef __cplusplus
}
#endif
