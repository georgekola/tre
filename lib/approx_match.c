/*
 * =====================================================================================
 *
 *       Filename:  approx_match.c
 *
 *    Description:  Shared library takes as input
 *                  1. int max_errors 2. char *needle 3. char *haystack and 
 *                 does approximate match of needle in haystack
 *
 *        Version:  1.0
 *        Created:  02/01/2013 12:27:49 PM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  George Kola (), georgekola@gmail.com
 *        Company:  
 *
 * =====================================================================================
 */
#include <tre/tre.h>
#include <string.h>
#define likely(x) __builtin_expect((x),1)
#define unlikely(x) __builtin_expect((x),0)


int approx_match(unsigned int max_err, const char *regex, const char *haystack){
    regex_t preg;	  /* Compiled pattern to search for. */
    size_t search_str_len=strlen(regex);
    int cflags= REG_ICASE;
    if(unlikely(tre_regncomp(&preg,regex,search_str_len,cflags)!=0)){
        return -2;
    }
    regamatch_t match;
    memset(&match, 0, sizeof(match));
    regaparams_t match_params;
    tre_regaparams_default(&match_params);
    match_params.max_cost=(int)max_err;
    int errcode=tre_reganexec(&preg,haystack,strlen(haystack),&match,match_params,0);
    tre_regfree(&preg);
    if(errcode==0){
        return match.cost;
    }
    return -1;
}

