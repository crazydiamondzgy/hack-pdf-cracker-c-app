#include "pdfparser.h"
#include <stdlib.h>
#include <string.h>

#define BUFFSIZE 256

/** Please rewrite all of this file in a clean and stable way */

struct p_str {
  uint8_t *content;
  uint8_t len;
};

typedef struct p_str p_str;

int isWhiteSpace(const int ch) {
  return (ch == 0x20 || (ch >= 0x09 && ch <= 0x0d) || ch == 0x00);
}

int
isDelimiter(const int ch) {
  switch(ch) {
  case '(':
  case ')':
  case '<':
  case '>':
  case '[':
  case ']':
  case '{':
  case '}':
  case '/':
  case '%':
    return 1;
  default:
    return 0;
  }
}

int
isEndOfLine(const int ch) {
  return (ch == 0x0a || ch == 0x0d);
}

static int
parseIntWithC(FILE *file, const int c) {
  int neg = 0;
  int i = 0;
  int ch = c;

  if(ch == '-') {
    neg = 1;
    ch = getc(file);
  }
  else if(ch == '+')
    ch = getc(file);
  while(ch >= '0' && ch <= '9') {
    i *= 10;
    i += ch-'0';
    ch = getc(file);
  }
  ungetc(ch,file);
  if(neg)
    i *= -1;

  return i;
}

static int
parseInt(FILE *file) {
  return parseIntWithC(file, getc(file));
}


static char
parseWhiteSpace(FILE *file) {
  int ch;
  do {
    ch = getc(file);
  } while(isWhiteSpace(ch));
  return ch;
}

static char*
parseName(FILE *file) {
  int ch;
  unsigned int i;
  char *ret;
  char buff[BUFFSIZE];

  ch = parseWhiteSpace(file);

  if(ch != '/') {
    ungetc(ch, file);
    return NULL;
  }
  ch = getc(file);
  for(i=0; i<BUFFSIZE && !isWhiteSpace(ch) && 
	!isDelimiter(ch) && ch != EOF; ++i) {
    buff[i] = ch;
    ch = getc(file);
  }
  ungetc(ch, file);
  buff[i++] = '\0';
  ret = malloc(sizeof(char)*i);
  memcpy(ret, buff, i);
  return ret;
}

/**
static int
isName(FILE *file, const char *str) {
  int ch;
  unsigned int i;

  ch = parseWhiteSpace(file);

  if(ch != '/') {
    ungetc(ch, file);
    return 0;
  }
  for(i=0; i<strlen(str); ++i) {
    ch = getc(file);
    if(ch != str[i])
      return 0;
  }
  return 1;
}
*/

static int
isWord(FILE *file, const char *str) {
  int ch;
  unsigned int i;
  for(i=0; i<strlen(str); ++i)
    if((ch = getc(file)) != str[i])
      goto ret;
  return 1;
 ret:
  ungetc(ch,file);
  return 0;
}

int
openPDF(FILE *file, EncData *e) {
  int ret = 0;
  int minor_v = 0, major_v = 0;
  if(getc(file) == '%' && getc(file) == 'P' && getc(file) == 'D' 
     && getc(file) == 'F' && getc(file) == '-') {
    major_v = parseInt(file);
    if(getc(file) == '.')
      minor_v = parseInt(file);
    if(major_v >= 0)
      ret = 1;
  }

  if(ret) {
    e->version_major = major_v;
    e->version_minor = minor_v;
  } 
  return ret;
}

uint8_t
hexToInt(const int b) {
  if(b >= '0' && b <= '9')
    return b-'0';
  else if(b >= 'a' && b <= 'f')
    return b-'a'+10;
  else if(b >= 'A' && b <= 'F')
    return b-'A'+10;
  else
    return 0;
}

static p_str*
parseHexString(const uint8_t *buf, const unsigned int len) {
  unsigned int i,j;
  p_str *ret;
 
  ret = malloc(sizeof(p_str));
  ret->content = malloc(sizeof(uint8_t)*(len/2));
  ret->len = (len/2);

  for(i=0, j=0; i<len; i += 2) {
    ret->content[j] = hexToInt(buf[i]) * 16;
    ret->content[j] += hexToInt(buf[i+1]);
    j++;
  }

  return ret;
}

static p_str*
objStringToByte(const uint8_t* str, const unsigned int len) {
  unsigned int i, j, l;
  uint8_t b, d;
  uint8_t *tmp;
  p_str *ret;
  tmp = (uint8_t *)malloc(len);

  for(i=0, l=0; i<len; i++, l++) {
    b = str[i];
    if(b == '\\') {
      /** 
       * We have reached a special character or the beginning of a octal 
       * up to three digit number and should skip the initial backslash
       **/
      i++;
      switch(str[i]) {
      case 'n':
	b = 0x0a;
	break;
      case 'r':
	b = 0x0d;
	break;
      case 't':
	b = 0x09;
	break;
      case 'b':
	b = 0x08;
	break;
      case 'f':
	b = 0x0c;
	break;
      case '(':
	b = '(';
	break;
      case ')':
	b = ')';
	break;
      case '\\':
	b = '\\';
	break;
      default:
	if(str[i] >= '0' && str[i] < '8') {
	  d = 0;
	  for(j=0; i < len && j < 3 &&
		str[i] >= '0' && str[i] < '8' &&
		(d*8)+(str[i]-'0') < 256; j++, i++) {
	    d *= 8;
	    d += (str[i]-'0');
	  }
	  /** 
	   * We need to step back one step if we reached the end of string
	   * or the end of digits (like for example \0000)
	   **/
	  if(i < len || j < 3) {
	    i--;
	  }

	  b = d;
	}
      }
    }
    tmp[l] = b;
  }

  ret = malloc(sizeof(p_str));
  ret->content = malloc(sizeof(uint8_t)*(l));
  ret->len = l-1;

  memcpy(ret->content, tmp, l);
  free(tmp);
  return ret;
}

static p_str*
parseRegularString(FILE *file) {
  unsigned int len, p;
  int ch;
  p_str *ret;
  uint8_t buf[BUFFSIZE];
  int skip = 0;

  ch = parseWhiteSpace(file);
  if(ch == '(') {
    p = 1;
    ch = getc(file);
    for(len=0; len < BUFFSIZE && p > 0 && ch != EOF; len++) {
      buf[len] = ch;
      if(skip == 0) {
	if(ch == '(')
	  p++;
	else if(ch == ')')
	  p--;
	if(ch == '\\')
	  skip = 1;
      }
      else
	skip = 0;
      ch = getc(file);
    }
    ungetc(ch, file);
    ret = objStringToByte(buf, len);
  }
  else if(ch == '<') {
    len = 0;
    while(ch != '>' && len < BUFFSIZE && ch != EOF) {
      if((ch >= '0' && ch <= '9') || 
	 (ch >= 'a' && ch <= 'f') || 
	 (ch >= 'A' && ch <= 'F')) {
	buf[len++] = ch;
      }
      ch = getc(file);
    }
    ungetc(ch,file);
    ret = parseHexString(buf,len);
  }
  else
    ret = NULL;
return ret;
}

static int
findTrailer(FILE *file, EncData *e) {
  int ch;
  /**  int pos_i; */
  int encrypt = 0;
  int id = 0;
  int e_pos = -1;
  p_str *str = NULL;
  
  ch = getc(file);
  while(ch != EOF) {
    if(isEndOfLine(ch)) {
      if(isWord(file, "trailer")) {
	/**	printf("found trailer\n");*/
	ch = parseWhiteSpace(file);
	if(ch == '<' && getc(file) == '<') {
	  /** we can be pretty sure to have found the trailer.
	      start looking for the Encrypt-entry */

	  /**
	  pos_i = ftell(file);
	  printf("found Trailer at pos %x\n", pos_i);
	  */
	  ch = getc(file);
	  while(ch != EOF) {
	    if(ch == '>') {
	      ch = getc(file);
	      if(ch == '>')
		break;
	    }
	    while(ch != '/' && ch != EOF) {
	      ch = getc(file);
	    }
	    ch = getc(file);
	    /**printf("found a name: %c\n", ch);*/
	    if(e_pos < 0 && ch == 'E' && isWord(file, "ncrypt")) {
	      e_pos = parseIntWithC(file,parseWhiteSpace(file));
	      if(e_pos >= 0) {
		/**
		   pos_i = ftell(file);
		   printf("found Encrypt at pos %x, ", pos_i);
		   printf("%d\n", e_pos);
		*/
		encrypt = 1;
	      }
	    }
	    else if(ch == 'I' && getc(file) == 'D') {
	      ch = parseWhiteSpace(file);
	      while(ch != '[' && ch != EOF)
		ch = getc(file);

	      if(str) {
		if(str->content)
		  free(str->content);
		free(str);
	      }

	      str = parseRegularString(file);
	      /**
	      pos_i = ftell(file);
	      printf("found ID at pos %x\n", pos_i);
	      */
	      if(str)
		id = 1;
	      ch = getc(file);
	    }
	    else
	      ch = getc(file);
	    if(encrypt && id) {
	      /**printf("found all, returning: epos: %d\n",e_pos);*/
	      e->fileID = str->content;
	      e->fileIDLen = str->len;
	      free(str);
	      return e_pos;
	    }
	  }
	}  
      }
      else {
	ch = getc(file);
      }
    }
    else
      ch = getc(file);
  }
  /**  printf("finished searching\n");*/

  if(str) {
    if(str->content)
      	free(str->content);
    free(str);
  }

  if(!encrypt && id)
      return ETRENF;
  else if(!id && encrypt)
    return ETRINF;
  else 
    return ETRANF;
}

static int
parseEncrypObject(FILE *file, EncData *e) {
  int ch, dict = 1;
  int fe = 0;
  int ff = 0;
  int fl = 0;
  int fo = 0;
  int fp = 0;
  int fr = 0;
  int fu = 0;
  int fv = 0;
  p_str *str = NULL;

  ch = getc(file);
  while(ch != EOF) {
    if(ch == '>') {
      ch = getc(file);
      if(ch == '>') {
	dict--;
	if(dict <= 0)
	   break;
      }
    }
    else if(ch == '<') {
      ch = getc(file);
      if(ch == '<') {
	dict++;
      }
    }
    if(ch == '/') {
      ch = getc(file);
      switch(ch) {
      case 'E':
	if(isWord(file, "ncryptMetadata")) {
	  ungetc(parseWhiteSpace(file), file);
	  if(isWord(file, "0"))
	    fe = 1;
	}
	break;
      case 'F':
	if(isWord(file, "ilter")) {
	  char *s_handler = parseName(file);
	  if(s_handler != NULL) {
	    e->s_handler = s_handler;
	    ff = 1;
	  }
	  break;
	}
      case 'L':
	if(isWord(file, "ength")) {
	  int tmp_l = parseIntWithC(file,parseWhiteSpace(file));
	  if(!fl) { 
	    /* BZZZT!!  This is sooo wrong but will work for most cases.
	       only use the first length we stumble upon */
	    e->length = tmp_l;
	  }
	  fl = 1;
	}
	break;
      case 'O':
	str = parseRegularString(file);
	if(!str)
	  break;
	if(str->len != 32)
	  fprintf(stderr, "WARNING: O-String != 32 Bytes: %d\n", str->len);
	e->o_string = str->content;
	free(str);
	fo = 1;
	break;
      case 'P':
	ch = getc(file);
	if(isWhiteSpace(ch)) {
	  ch = parseWhiteSpace(file);
	  e->permissions = parseIntWithC(file,ch);
	  fp = 1;
	}
	break;
      case 'R':
	ch = getc(file);
	if(isWhiteSpace(ch)) {
	  ch = parseWhiteSpace(file);
	  e->revision = parseIntWithC(file,ch);
	  fr = 1;
	}
	break;
      case 'U':
	str = parseRegularString(file);
	if(!str)
	  break;
	if(str->len != 32)
	  fprintf(stderr, "WARNING: U-String != 32 Bytes: %d\n", str->len);
	e->u_string = str->content;
	free(str);
	fu = 1;
	break;
      case 'V':
	ch = getc(file);
	if(isWhiteSpace(ch)) {
	  e->version = parseIntWithC(file, parseWhiteSpace(file));
	  fv = 1;
	}
	break;
      default:
	break;
      }
    }
    ch = parseWhiteSpace(file);
  }

  if(!fe)
    e->encryptMetaData = 1;
  if(!fl)
    e->length = 40;
  if(!fv)
    e->version = 0;

  if(strcmp(e->s_handler,"Standard") != 0)
    return 1;

  return ff & fo && fp && fr && fu;
}

/** 
    This is not a really definitive search.
    Should be replaced with something better
*/
static int
findEncryptObject(FILE *file, const int e_pos, EncData *e) {
  int ch;
  int pos_i;

  /** only find the encrypt object if e_pos > -1 */
  if(e_pos < 0)
    return 0;

  ch = getc(file);
  while(ch != EOF) {
    if(isEndOfLine(ch)) {
      if(parseInt(file) == e_pos) {
	ch = parseWhiteSpace(file);
	if(ch >= '0' && ch <= '9') {
	  ch = parseWhiteSpace(file);
	  if(ch == 'o' && getc(file) == 'b' && getc(file) == 'j' &&
	     parseWhiteSpace(file) == '<' && getc(file) == '<') {
	    pos_i = ftell(file);
	    return parseEncrypObject(file, e);
	  }
	}
      }
    }
    ch = getc(file);
  }
  return 0;
}


int
getEncryptedInfo(FILE *file, EncData *e) {
  int e_pos = -1;
  int ret;

  if(fseek(file, 0L, SEEK_END-1024))
    e_pos = findTrailer(file, e);
  if(e_pos < 0) {
    rewind(file);
    e_pos = findTrailer(file, e);
  }
  if(e_pos < 0) {
    return e_pos;
  }
  rewind(file);
  ret = findEncryptObject(file, e_pos, e);
  if(!ret)
    return EENCNF;

  return 0;
}
