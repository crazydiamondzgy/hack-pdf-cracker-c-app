#ifndef _PDFPARSER_H_
#define _PDFPARSER_H_
#include <stdio.h>
#include "common.h"

#define EENCNF -1 /* Encryption Object Not Found */
#define ETRANF -2 /* Trailer Information Not Found */
#define ETRENF -3 /* Trailer: Encryption Object Not Found */
#define ETRINF -4 /* Trailer: FileID Object Not Found */

int openPDF(FILE *file, EncData *e);
int getEncryptedInfo(FILE *file, EncData *e);


#endif /** _PDFPARSER_H_ */
