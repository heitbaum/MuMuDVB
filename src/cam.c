#include "cam.h"
#include "mumudvb.h"

extern unsigned long       crc32_table[256];

int cam_parse_pmt(unsigned char *buf, mumudvb_pmt_t *pmt, struct ca_info *cai)
{

  //NOTE IMPORTANTE : normalement quand un paquet est d�coup� il n'y a pas d'autres pids entre donc on stoque un seul mumudvb_pmt
  ts_header_t *header;
  int ok=0;
  int parsed=0;
  int delta,pid,k,l;
  
  header=(ts_header_t *)buf;
  pid=HILO(header->pid);
  //printf("#Nouveau paquet, TS_Header : PID : %d payload_start %x\n",HILO(header->pid), header->payload_unit_start_indicator);
  //if(header->payload_unit_start_indicator) //Cet entete indique que c'est un nouveau paquet
  //{
  delta = TS_HEADER_LEN-1; //ce delta permet de virer l'en tete et de ne r�cup�rer que les donn�es
  
  //Le adaptation field est un champ en plus, s'il est pr�sent il faut le sauter pour acc�der aux vraies donn�es
  if (header->adaptation_field_control & 0x2)
    {
      printf("\t#####Adaptation field \n");
      delta += buf[delta] ;        // add adapt.field.len
    }
  else if (header->adaptation_field_control & 0x1) {
    //printf("\t#No adaptation field \n");
    if (buf[delta]==0x00 && buf[delta+1]==0x00 && buf[delta+2]==0x01) {
      // -- PES/PS                                                                                                                               
      //tspid->id   = buf[j+3];                                                                                                                  
      printf("\t#PES/PS ----- We ignore \n");
      ok=0;
    } else {
      //if(buf[delta]){                                                                                                                          
      //J'ai pas trop compris ce qu'il faut faire dans ce cas --> on se casse (ps : il arrive quasiment jamais)                                
      //printf("\t#probleme tag : abebueb\n");                                                                                                 
      //ok=0;                                                                                                                                  
      //}else{                                                                                                                                   
      //Tout va bien, on skippe le z�ro en tro
      //delta += 1;                                                                                                                            
      ok=1;
      //}                                                                                                                                        
    }
  }
  if (header->adaptation_field_control == 3) { ok=0; }
  if(header->payload_unit_start_indicator) //Cet entete indique que c'est un nouveau paquet
    {
      if(ok)
	{
	  //On regarde si on a d�ja un paquet en stoc
	  //Si oui, on le traite
	  if(! pmt->empty){
	    if(pmt->pid==pid && pmt->continuity_counter==header->continuity_counter)
	      printf("###########DOUBLON\n\n\n");
	    else
	      {
		//printf("###########ON Parse le pid %d act_len : %d\n",pmt->pid,pmt->len);
#if 0
		if(pmt->len>188)
		  {
		    printf("Voici la tete du paquet : \n");                                                                                        
		    for(l=0;(20*l)<pmt->len;l++){
		      for(k=0;(k<20)&&(k+20*l)<pmt->len;k++)
			printf("%02x.",pmt->packet[k+20*l]);
		      printf("\n");
		    }
		    printf("\n\n");                                                                                                                
		    //for(k=0;k<pmt->len;k++)
		    //printf("%c",pmt->packet[k]);
		    //printf("\n\n");
		  }
#endif
		parsed=cam_send_ca_pmt(pmt,cai); //ICI on regarde le paquet
	      }
	  }
	  pmt->empty=0;
	  pmt->continuity_counter=header->continuity_counter;
	  pmt->pid=pid;
	  pmt->len=AddPacketStart(pmt->packet,buf+delta+1,188-delta-1); //on ajoute le paquet //NOTE len                                                   
	}
    }
  else if(header->payload_unit_start_indicator==0)
    {
      //c'est pas un premier paquet                                                                                                                  
      // -- pid change in stream? (without packet start)                                                                                             
      // -- This is currently not supported   $$$ TODO                                                                                               
      if (pmt->pid != pid) {
	printf("ERRREUR : CHANGEMENT DE PID\n");
	pmt->empty=1;
      }
      // -- discontinuity error in packet ?                                                                                                          
      if  ((pmt->continuity_counter+1)%16 != header->continuity_counter) {
	printf("ERRREUR : Erreur de discontinuit�\n\n");
	pmt->empty=1;
      }
      pmt->continuity_counter=header->continuity_counter;
      // -- duplicate packet?  (this would be ok, due to ISO13818-1)                                                                                 
      //if ((pid == tsd.pid) && (cc == tsd.continuity_counter)) {                                                                                    
      //return 1;                                                                                                                                    
      //}
      //AAAAAATTTTTTTTTTTTTTEEEEEEEEEEEEEENNNNNNNNNNNNNTTTTTTTTTTTTTIIIIIIIIIIIIOOOOOOOOOOOOONNNNNNNNNNNNNNNN
      //Le delta n'est pas initialis� ici, regarder l'autre code, j'ai du zapper un }
      //COrrig�, ca devrait marcher
      pmt->len=AddPacketContinue(pmt->packet,buf+delta,188-delta,pmt->len); //on ajoute le paquet //NOTE len                                                
    }

  return parsed;

}


//Les fonctions qui permettent de coller les paquets les uns aux autres

//
// -- add TS data
// -- return: 0 = fail
//


int AddPacketStart (unsigned char *packet, unsigned char *buf, unsigned int len)
{


  memset(packet,0,4096);
  //  printf("actuellement %d, on rajoute %d\n",act_len,len);
  //printf("on rajoute %d\n",len);
  memcpy(packet,buf,len);

  return len;
}

int AddPacketContinue  (unsigned char *packet, unsigned char *buf, unsigned int len, unsigned int act_len)
{

  //printf("actuellement %d, on rajoute %d\n",act_len,len);
  memcpy(packet+act_len,buf,len);

  return len+act_len;

}



int cam_send_ca_pmt( mumudvb_pmt_t *pmt, struct ca_info *cai)
{
  pmt_t *pmt_struct;
  unsigned long crc32;
  int i;

  if(!cai->initialized)
    return 0;

  printf("paquet PMT PID %d len %d\n",pmt->pid, pmt->len);


  pmt_struct=(pmt_t *)pmt->packet;

  printf("\tPMT header  section_len %d \n", HILO(pmt_struct->section_length));

  //the real lenght
  pmt->len=HILO(pmt_struct->section_length)+3; //+3 pour les trois bits du d�but (compris le section_lenght)


  //CRC32
  //CRC32 calculation taken from the xine project
  //Test of the crc32
  crc32=0xffffffff;
  //we compute the CRC32
  //we have two ways: either we compute untill the end and it should be 0
  //either we exclude the 4 last bits and in should be equal to the 4 last bits
  for(i = 0; i < pmt->len; i++) {
    crc32 = (crc32 << 8) ^ crc32_table[(crc32 >> 24) ^ pmt->packet[i]];
  }
  
  if(crc32!=0)
    {
      //Bad CRC32
      printf("paquet PMT BAD CRC32\n");
      return 0; //We don't send this PMT
    }

  convert_pmt(cai, pmt, 3, 1);

    //TODO : regarder le tampon de sortie

    //TODO l'envoyer et envoyer le query pour tester

  //on retourne 0 pour le moment (on ne retourne 1 que quand on sera sur que c'est ok)
  return 1;

}


//Code from libdvb

int convert_desc(struct ca_info *cai, 
		 uint8_t *out, uint8_t *buf, int dslen, uint8_t cmd)
{
  int i, j, dlen, olen=0;
  int id;

  out[2]=cmd;                            //ca_pmt_cmd_id 01 ok_descrambling 02 ok_mmi 03 query 04 not_selected
  for (i=0; i<dslen; i+=dlen) {          //loop on all the descriptors (for each descriptor we add its length)
    dlen=buf[i+1]+2;                     //ca_descriptor len
    if ((buf[i]==9)&&(dlen>2)&&(dlen+i<=dslen)) { //here buf[i]=descriptor_tag (9=ca_descriptor)
      id=(buf[i+2]<<8)|buf[i+3];
      printf("New CA descriptor id 0x%x %d\n",id,id);
      for (j=0; j<cai->sys_num; j++)
	if (cai->sys_id[j]==((buf[i+2]<<8)|buf[i+3])) //does the system id supported by the cam ?
	  break; //yes --> we leave the loop
      if (j==cai->sys_num) // do we leaved the loop just because we reached the end ?
	continue;          //yes, so we dont record this descriptor
      printf("\tOK CA descriptor\n");
      memcpy(out+olen+3, buf+i, dlen); //good descriptor supported by the cam, we copy it
      olen+=dlen; //output let
    }
  }
  olen=olen?olen+1:0; //if not empty we add one
  out[0]=(olen>>8);   //we write the program info_len
  out[1]=(olen&0xff);
  return olen+2;      //we return the total written len
}

int convert_pmt(struct ca_info *cai, mumudvb_pmt_t *pmt, 
		       uint8_t list, uint8_t cmd)
{
	int slen, dslen, o, i;
	uint8_t *buf;
	uint8_t *out;
	
	buf=pmt->packet;
	out=pmt->converted_packet;
	//slen=(((buf[1]&0x03)<<8)|buf[2])+3; //section len (deja contenu dans mon pmt)
	slen=pmt->len;
	out[0]=list;   //ca_pmt_list_mgmt 00 more 01 first 02 last 03 only 04 add 05 update
	out[1]=buf[3]; //program number and version number
	out[2]=buf[4]; //program number and version number
	out[3]=buf[5]; //program number and version number
	dslen=((buf[10]&0x0f)<<8)|buf[11]; //program_info_length
	o=4+convert_desc(cai, out+4, buf+12, dslen, cmd); //new index : 4 + the descriptor size
	for (i=dslen+12; i<slen-9; i+=dslen+5) {      //we parse the part after the descriptors
	  dslen=((buf[i+3]&0x0f)<<8)|buf[i+4];        //ES_info_length
	  if ((buf[i]==0)||(buf[i]>4))                //stream_type
	    continue;
	  out[o++]=buf[i];                            //stream_type
	  out[o++]=buf[i+1];                          //reserved and elementary_pid
	  out[o++]=buf[i+2];                          //reserved and elementary_pid
	  o+=convert_desc(cai, out+o, buf+i+5, dslen, cmd);//we look to the descriptors associated to this stream
	}
	return o;
}

int ci_get_ca_info(int fd, int slot, struct ca_info *cai)
{
  cai->sys_num=0;
#if 0
	uint8_t buf[256], *p=buf+3;
	int len, i;

	ci_send_obj(fd, AOT_CA_INFO_ENQ, NULL, 0);
	if ((len=ci_getmsg(fd, slot, buf))<=8)
		return -1;
	printf("CA systems : ");
	len=ci_read_length(&p);
	cai->sys_num=p[0];
	for (i=0; i<cai->sys_num; i++) {
		cai->sys_id[i]=(p[1+i*2]<<8)|p[2+i*2];
		printf("%04x ", cai->sys_id[i]);
	}
	printf("\n");
#endif
	return 0;
}