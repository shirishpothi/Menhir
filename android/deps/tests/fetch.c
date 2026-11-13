#include "menhir_fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* rom DEST [CA_DIR], image URL DEST [CA_DIR] [CANCEL_AFTER_BYTES]. */
int main(int argc,char **argv) {
 if(argc<3)return 2;
 MenhirFetch *j=NULL;
 unsigned long long cancel_after=0;
 if(!strcmp(argv[1],"rom"))j=menhir_fetch_rom(argv[2],argc>3?argv[3]:NULL);
 else if(!strcmp(argv[1],"image")&&argc>=4) {
  j=menhir_fetch_ipsw(argv[2],argv[3],argc>4&&*argv[4]?argv[4]:NULL);
  if(argc>5)cancel_after=strtoull(argv[5],NULL,10);
 }
 if(!j)return 2;
 MenhirFetchProgress p;
 do { menhir_fetch_poll(j,&p);if(cancel_after&&p.downloaded>=cancel_after)menhir_fetch_cancel(j);usleep(1000); } while(p.state==MENHIR_FETCH_RUNNING);
 printf("state=%d downloaded=%llu total=%llu error=%s\n",p.state,(unsigned long long)p.downloaded,(unsigned long long)p.total,p.error);
 menhir_fetch_free(j);return p.state==MENHIR_FETCH_DONE?0:p.state==MENHIR_FETCH_CANCELLED?3:1;
}
