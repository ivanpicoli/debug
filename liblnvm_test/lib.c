#include <linux/lightnvm.h>
#include <stdio.h>

int main(){
	struct nvm_ioctl_info c;
	int ret, i;

	memset(&c, 0 sizeof(struct nvm_ioctl_info);

	ret = nvm_get_info(&c);

	printf("LightNVM version (%u, %u, %u). %u target type(s) registered.\n",c.version[0],c.version[1],c.version[2], c.tgtsize);

	for(i=0;i<c.tgtsize;i++){
		struct nvm_ioctl_tgt_info *tgt = &c.tgts[i];

		printf("	Type: %s (%u, %u, %u)\n", tgt->target.tgtname, tgt->version[0], tgt->version[1], tgt->version[2]);
	}
}
