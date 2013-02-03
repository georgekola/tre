
/*
 * =====================================================================================
 *
 *       Filename:  test.cc
 *
 *    Description:  Code to approximate match needle in a haystack
 *
 *        Version:  1.0
 *        Created:  01/30/2013 20:38:47
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  George Kola (), georgekola@gmail.com
 *        Company:  
 *
 * =====================================================================================
 */

#include <tre/tre.h>
int main(int argc, char *argv[]){
    char *regex=argv[1];
    char *haystack=argv[2];

    
    regex_t preg;	  /* Compiled pattern to search for. */

    //errcode = tre_regcomp(&preg, regexp, comp_flags);
    int search_str_len=strlen(regex);
    int cflags= REG_ICASE;
    ret = tre_compile(&preg, (const tre_char_t *)regex, search_str_len, cflags);
    //extern int
    //    tre_reganexec(const regex_t *preg, const char *string, size_t len,
    //            regamatch_t *match, regaparams_t params, int eflags);
    tre_reganexec(&preg,haystack,strlen(haystack));

    int errcode;
    regamatch_t match;
    regmatch_t pmatch[1];
    memset(&match, 0, sizeof(match));
    regaparams_t match_params;
    memset(&match_params,0,sizeof(match_params));


}



