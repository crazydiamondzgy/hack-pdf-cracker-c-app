#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <windows.h>

#include "..\utils.h"
#include "pdfcrack.h"
#include "pdfparser.h"
#include "md5.h"
#include "rc4.h"

/** sets the number of bytes to decrypt for partial test in revision 3.
Three should be a good number for this as this mean a match should only
happen every 256^3=16777216 check and that should be unique enough to 
motivate a full retry on that entry.
*/
#define PARTIAL_TEST_SIZE 3

static const uint8_t
pad[32] = {
	0x28,0xBF,0x4E,0x5E,0x4E,0x75,0x8A,0x41,
		0x64,0x00,0x4E,0x56,0xFF,0xFA,0x01,0x08,
		0x2E,0x2E,0x00,0xB6,0xD0,0x68,0x3E,0x80,
		0x2F,0x0C,0xA9,0xFE,0x64,0x53,0x69,0x7A
};

/** buffers for stuff that we can precompute before the actual cracking */
static uint8_t *encKeyWorkSpace;
static uint8_t password_user[33];
static uint8_t *rev3TestKey;
static unsigned int ekwlen;
static char true_password[64];
static char user_password[64];

/** points to the current password in clear-text */
static uint8_t *currPW;
/** current length of the password we are working with */
static unsigned int currPWLen;

/** statistics */
static unsigned int nrprocessed;
static time_t startTime;

/** pointer to the actual encoding-data from the pdf */
static const EncData *encdata;

/** some configuration switches */
static int crackDone;
static int knownPassword;
static int workWithUser;

/** Print out some statistics */
int
printProgress(void) {
	time_t currentTime;
	char str[33];
	
	if(crackDone)
		return 1;
	
	currentTime = time(NULL);
	memcpy(str,currPW,currPWLen);
	str[currPWLen] = '\0';
	printf("Average Speed: %.1f w/s. ",
		nrprocessed/difftime(currentTime,startTime));
	printf("Current Word: '%s'\n",str);
	fflush(stdout);
	nrprocessed = 0;
	startTime = time(NULL);
	return 0;
}

/**
* Initialisation of the encryption key workspace to manage a bit faster 
* switching between keys
*/
static unsigned int
initEncKeyWorkSpace(const int revision, const int encMetaData,
					const int permissions, const uint8_t *ownerkey,
					const uint8_t *fileID, const unsigned int fileIDLen) {
					/** 
					*   Algorithm 3.2 Computing an encryption key (PDF Reference, v 1.7, p.125)
					*
					*   Make space for:
					*   field           | bytes
					*   -----------------------
					*   padded password | 32
					*   O entry         | 32
					*   P entry         |  4
					*   fileID          | <fileIDLEn>
					*  [extra padding]  | [4] (Special for step 6)
	**/
	unsigned int size = (revision > 3 && !encMetaData) ? 72 : 68;
	encKeyWorkSpace = malloc(size + fileIDLen);
	
	/** Just to be sure we have no uninitalized stuff in the workspace */
	memcpy(encKeyWorkSpace, pad, 32);
	
	/** 3 */ 
	memcpy(encKeyWorkSpace + 32, ownerkey, 32);
	
	/** 4 */
	encKeyWorkSpace[64] = permissions & 0xff;
	encKeyWorkSpace[65] = (permissions >> 8) & 0xff;
	encKeyWorkSpace[66] = (permissions >> 16) & 0xff;
	encKeyWorkSpace[67] = (permissions >> 24) & 0xff;
	
	/** 5 */
	memcpy(encKeyWorkSpace + 68, fileID, fileIDLen);
	
	/** 6 */
	if(revision > 3 && !encMetaData) {
		encKeyWorkSpace[68+fileIDLen] = 0xff;
		encKeyWorkSpace[69+fileIDLen] = 0xff;
		encKeyWorkSpace[70+fileIDLen] = 0xff;
		encKeyWorkSpace[71+fileIDLen] = 0xff;
	}
	
	return size+fileIDLen;
}

/** toupper(3) but expanded to handle iso-latin-1 characters */
static uint8_t
isolat1ToUpper(const uint8_t b) {
	if(unlikely(b >= 0xe0 && b <= 0xf6))
		return b-0x20;
	else
		return toupper(b);
}

/** Really stupid permutate that needs to be replaced with a better framwork
for smart permutations of the current password */
static int
do_permutate(void) {
	static int ret = 0;
	uint8_t tmp;
	
	tmp = isolat1ToUpper(currPW[0]);
	if(tmp != currPW[0]) {
		currPW[0] = tmp;
		ret = !ret;
	}
	else
		ret = 0;  
	
	return ret;
}

/** Dummy-function to use when no permutations are wanted */
static int
no_permutate(void) { return 0; }

/** Placeholder for the correct permutation-function to run */
static int (*permutate)() = NULL;

/** Common handling of the key for all rev3-functions */
#define RC4_DECRYPT_REV3(n) {			\
    for(i = 19; i >= 0; --i) {			\
	for(j = 0; j < length; ++j)		\
	tmpkey[j] = enckey[j] ^ i;		\
	rc4Decrypt(tmpkey, test, n, test);	\
    }						\
}

/** Checks if the rev2-password set up in encKeyWorkSpace is the correct one
and return 1 if it is and 0 otherwise.
*/
static int
isUserPasswordRev2(void) {
	uint8_t enckey[16];
	
	md5(encKeyWorkSpace, ekwlen, enckey);
	
	return rc4Match40b(enckey, encdata->u_string, pad);
}

/** Checks if the rev3-password set up in encKeyWorkSpace is the correct one
and return 1 if it is and 0 otherwise.
*/
static int
isUserPasswordRev3(void) {
	uint8_t test[16], enckey[16], tmpkey[16];
	int i;
	unsigned int length, j;
	
	length = encdata->length/8;
	md5(encKeyWorkSpace, ekwlen, enckey);      
	md5_50(enckey);
	memcpy(test, encdata->u_string, 16);
	
	RC4_DECRYPT_REV3(PARTIAL_TEST_SIZE);
	
	/** if partial test succeeds we make a full check to be sure */
	if(unlikely(memcmp(test, rev3TestKey, PARTIAL_TEST_SIZE) == 0)) {
		memcpy(test, encdata->u_string, 16);
		RC4_DECRYPT_REV3(16);
		if(memcmp(test, rev3TestKey, 16) == 0) {
			return 1;
		}
	}
	return 0;
}

int
runCrackRev2_o(char *string, int len) {
	uint8_t enckey[16];
	unsigned int lpasslength;
	
	lpasslength = 0;
	
	strcpy(currPW, string);
	currPWLen = len;
	memcpy(currPW + currPWLen, pad, 32-currPWLen);
    do {
		md5(currPW, 32, enckey);
		
		rc4Decrypt(enckey, encdata->o_string, 32, encKeyWorkSpace);
		md5(encKeyWorkSpace, ekwlen, enckey);
		if(rc4Match40b(enckey, encdata->u_string, pad)) {
			memcpy(password_user, encKeyWorkSpace, 32);
			return 1;
		}
		
		++nrprocessed;
    } while(permutate());
	
	return 0;
}

int
runCrackRev3_o(char *string, int len) {
	uint8_t test[32], enckey[16], tmpkey[16];
	unsigned int j, length, lpasslength;
	int i;
	
	length = encdata->length/8;
	lpasslength = 0;
	
	strcpy(currPW, string);
	currPWLen = len;
	memcpy(currPW + currPWLen, pad, 32-currPWLen);
    do {
		md5(currPW, 32, enckey);
		
		md5_50(enckey);
		
		memcpy(test, encdata->o_string, 32);
		RC4_DECRYPT_REV3(32);
		memcpy(encKeyWorkSpace, test, 32);
		
		if(isUserPasswordRev3()) {
			memcpy(password_user, encKeyWorkSpace, 32);
			return 1;
		}
		
		++nrprocessed;
    } while(permutate());
	
	return 0;
}

int
runCrackRev2_of(char *string, int len) {
	uint8_t enckey[16];
	unsigned int lpasslength;
	
	lpasslength = 0;
	
	strcpy(currPW, string);
	currPWLen = len;
	memcpy(currPW + currPWLen, pad, 32-currPWLen);
    do {
		md5(encKeyWorkSpace, 32, enckey);
		
		/* Algorithm 3.4 reversed */
		if(rc4Match40b(enckey, encdata->o_string, password_user))
			return 1;
		
		++nrprocessed;
    } while(permutate());
	
	return 0;
}

int
runCrackRev3_of(char *string, int len) {
	uint8_t test[32], enckey[16], tmpkey[16];
	unsigned int j, length, lpasslength;
	int i;
	
	length = encdata->length/8;
	lpasslength = 0;
	
	strcpy(currPW, string);
	currPWLen = len;
	memcpy(currPW + currPWLen, pad, 32-currPWLen);
	
    do {
		md5(encKeyWorkSpace, 32, enckey);
		
		md5_50(enckey);
		
		memcpy(test, encdata->o_string, 32);
		RC4_DECRYPT_REV3(PARTIAL_TEST_SIZE);
		
		/** if partial test succeeds we make a full check to be sure */
		if(unlikely(memcmp(test, password_user, PARTIAL_TEST_SIZE) == 0)) {
			memcpy(test, encdata->o_string, 32);
			RC4_DECRYPT_REV3(32);
			if(memcmp(test, password_user, 32) == 0)
				return 1;
		}
		
		++nrprocessed;
    } while(permutate());
	
	return 0;
}

int
runCrackRev3(char *string, int len) {
	uint8_t test[16], enckey[16], tmpkey[16];
	unsigned int j, length, lpasslength;
	int i;
	
	length = encdata->length/8;
	lpasslength = 0;
	startTime = time(NULL);
	
	strcpy(currPW, string);
	currPWLen = len;
	memcpy(currPW + currPWLen, pad, 32-currPWLen);
	
    do {
		md5(encKeyWorkSpace, ekwlen, enckey);
		
		md5_50(enckey);
		memcpy(test, encdata->u_string, 16);
		
		/** Algorithm 3.5 reversed */  
		RC4_DECRYPT_REV3(PARTIAL_TEST_SIZE);
		
		/** if partial test succeeds we make a full check to be sure */
		if(unlikely(memcmp(test, rev3TestKey, PARTIAL_TEST_SIZE) == 0)) {
			memcpy(test, encdata->u_string, 16);
			RC4_DECRYPT_REV3(16);
			if(memcmp(test, rev3TestKey, 16) == 0)
				return 1;
		}
		
		++nrprocessed;
    } while(permutate());
	
	return 0;
}


int
runCrackRev2(char *string, int len) {
	uint8_t enckey[16];
	unsigned int lpasslength;
	
	lpasslength = 0;
	strcpy(currPW, string);
	currPWLen = len;
	memcpy(currPW + currPWLen, pad, 32-currPWLen);
	
    do {
		md5(encKeyWorkSpace, ekwlen, enckey);
		
		/* Algorithm 3.4 reversed */
		if(rc4Match40b(enckey, encdata->u_string, pad))
			return 1;
		
		++nrprocessed;
    } while(permutate());
	
	return 0;
}

int runCrack(char *string, int len) {
	int found = 0;
	uint8_t cpw[32];
	
	if(!workWithUser && !knownPassword) {
		memcpy(cpw, pad, 32);
		currPW = cpw;
		if(encdata->revision == 2)
			found = runCrackRev2_o(string, len);
		else
			found = runCrackRev3_o(string, len);
	}  
	else if(encdata->revision == 2) {
		if(workWithUser)
			found = runCrackRev2(string, len);
		else /** knownPassword */
			found = runCrackRev2_of(string, len);
	}
	else {
		if(workWithUser)
			found = runCrackRev3(string, len);
		else /** knownPassword */
			found = runCrackRev3_of(string, len);
	}
	
	if(!found)
		return 0;
	else
		return 1;
}

/** returns the number of processed passwords */
unsigned int
getNrProcessed(void) { return nrprocessed; }

/** These are shared variables between loading and initialisation and controls
how to do the initialisation. Should not be touched by anything except 
loadState and cleanPDFCrack.
*/
static int recovery = 0;
static int permutation = 0;


/** cleans up everything as is needed to do a any initPDFCrack-calls after the
first one.
*/
void
cleanPDFCrack(void) {
	if(rev3TestKey) {
    /** Do a really ugly const to non-const cast but this one time it should
	be safe
		*/
		free((uint8_t*)rev3TestKey);
		rev3TestKey = NULL;
	}
	if(encKeyWorkSpace) {
		free(encKeyWorkSpace);
		encKeyWorkSpace = NULL;
	}
	knownPassword = 0;
	recovery = 0;
	permutation = 0;
}

/** initPDFCrack is doing all the initialisations before you are able to call
runCrack(). Make sure that you run cleanPDFCrack before you call this 
after the first time.
*/
int
initPDFCrack(const EncData *e, const uint8_t *upw, const int user, const int perm) {
	uint8_t buf[128];
	unsigned int upwlen;
	uint8_t *tmp;
	
	ekwlen = initEncKeyWorkSpace(e->revision, e->encryptMetaData, e->permissions,
		e->o_string, e->fileID, e->fileIDLen);
	
	encdata = e;
	currPW = encKeyWorkSpace;
	currPWLen = 0;
	nrprocessed = 0;
	workWithUser = user;
	crackDone = 0;
	setrc4DecryptMethod((const unsigned int)e->length);
	if(upw) {
		upwlen = strlen((const char*)upw);
		if(upwlen > 32)
			upwlen = 32;
		memcpy(password_user, upw, upwlen);
		memcpy(password_user+upwlen, pad, 32-upwlen);
		memcpy(encKeyWorkSpace, password_user, 32);
		knownPassword = 1;
	}
	/** Workaround to set password_user when loading state from file */
	if(recovery) 
		memcpy(encKeyWorkSpace, password_user, 32);
	
	if(encdata->revision == 2) {
		if(knownPassword) {
			if(!isUserPasswordRev2())
				return 0;
			memcpy(encKeyWorkSpace, pad, 32);
		}
		else {
			memcpy(password_user, pad, 32);
			knownPassword = isUserPasswordRev2();
		}
	}
	else if(e->revision >= 3) {
		memcpy(buf, pad, 32);
		memcpy(buf + 32, e->fileID, e->fileIDLen);
		tmp = malloc(sizeof(uint8_t)*16);
		md5(buf, 32+e->fileIDLen, tmp);
		rev3TestKey = tmp;
		if(knownPassword) {
			if(!isUserPasswordRev3())
				return 0;
			memcpy(encKeyWorkSpace, pad, 32);
		}
		else {
			memcpy(password_user, pad, 32);
			knownPassword = isUserPasswordRev3();
		}
	}
	
	permutation = (perm || permutation);
	if(permutation)
		permutate = do_permutate;
	else
		permutate = no_permutate;
	
	return 1;
}

static EncData e;

static int 
pdf_open(CrackContext *ctx)
{
	FILE *fp;
	
	uint8_t *userpassword = NULL;

	if((fp = fopen(ctx->input_filename, "rb")) == 0) {
		exit(-1);
	}
	
	openPDF(fp, &e);
	
	if(getEncryptedInfo(fp, &e))
	{
		exit(-1);
	}
	
	if(!initPDFCrack(&e, userpassword, 0, 0)) {
		cleanPDFCrack();
		exit(-1);
	}

	fclose(fp);
	return 0;
}

static int first_time = 1;

static int 
pdf_crack(CrackContext *ctx, char *string, unsigned int len)
{
	if (runCrack(string, strlen(string)) && true_password[0] == 0)
	{
		strncpy(ctx->pw, string, 64);
		return 1;
	}

	if (ctx->pw[0] && first_time)
	{
		FILE *fp;
		uint8_t *userpassword = NULL;
		first_time = 0;

		if((fp = fopen(ctx->input_filename, "rb")) == 0) {
			exit(-1);
		}
		
		openPDF(fp, &e);
		
		if(getEncryptedInfo(fp, &e))
		{
			exit(-1);
		}
		
		if(!initPDFCrack(&e, userpassword, 1, 0)) {
			cleanPDFCrack();
			exit(-1);
		}
		
		fclose(fp);
		return -1;
	}

	if (ctx->pw[0] && !first_time)
	{
		if (runCrack(string, len))
		{
			strncpy(user_password, string, 64);
			strcat(user_password, " & ");
			strcat(user_password, true_password);
			return 1;
		}
	}

	return 0;
}
/*
int remove_pdf()
{
	char buf[1024];
	char ofname[MAX_PATH + 1];
	int status;
	
	//
	// pdf转换程序
	//
	
	TCHAR szFilePath[MAX_PATH + 1]; 
	GetModuleFileName(NULL, szFilePath, MAX_PATH); 
	(strrchr(szFilePath, '\\'))[1] = 0;
	strcat(szFilePath, "pdftk.exe");

	//
	// 输出文件目录
	//

	strcpy(ofname, ctx->input_filename);
	(strrchr(ofname, '\\'))[1] = 0;
	strcat(ofname, "foo.pdf");

	//
	// 开始转换
	//

	sprintf(buf, "%s \"%s\" input_pw %s output \"%s\" allow AllFeatures verbose dont_ask", \
		szFilePath, filepath, true_password, ofname);
	status = system (buf);
	
	if (status == EXIT_SUCCESS)
	{
		return 0;
	}
	else
		return -1;
}
*/
static int 
pdf_close(CrackContext *ctx)
{
	return 0;
}

Cracker pdf_cracker = 
{
	"matrix pdf cracker", 
	"pdf", 
	CRACK_TYPE_DICTIONARY | CRACK_TYPE_BRUTEFORCE, 
    CRACK_ID_PDF, 
	0, 
	pdf_open, 
	pdf_crack, 
	pdf_close, 
	NULL
};
