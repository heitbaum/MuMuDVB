/* 
 * mumudvb - UDP-ize a DVB transport stream.
 * 
 * (C) 2004-2009 Brice DUBOST
 * 
 * The latest version can be found at http://mumudvb.braice.net
 * 
 * Copyright notice:
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *     
 */

/**@file
 * @brief This file contains the function for rewriting the sdt pid
 *
 * The SDT rewrite is made to announce only the video stream associated with the channel in the SDT pid
 * It avoids to have ghost channels which can disturb the clients
 */

#include <stdlib.h>
#include <string.h>

#include "mumudvb.h"
#include "ts.h"
#include "rewrite.h"
#include "log.h"
#include <stdint.h>

extern uint32_t       crc32_table[256];


/** @brief, tell if the sdt have a newer version than the one recorded actually
 * In the SDT pid there is a field to say if the SDT was updated
 * This function check if it has changed (in order to rewrite the sdt only once)
 * General Note : in case it change during streaming it can be a problem ane we would have to deal with re-autoconfiguration
 * Note this function can give flase positive since it doesn't check the CRC32
 *
 *@param rewrite_vars the parameters for sdt rewriting 
 *@param buf : the received buffer
 */
int sdt_need_update(rewrite_parameters_t *rewrite_vars, unsigned char *buf)
{
  sdt_t       *sdt=(sdt_t*)(buf+TS_HEADER_LEN);
  ts_header_t *header=(ts_header_t *)buf;

  if(header->payload_unit_start_indicator) //It's the beginning of a new packet
    if((sdt->version_number!=rewrite_vars->sdt_version) && (sdt->table_id==0x42))
      {
	log_message(MSG_DEBUG,"SDT rewrite : Need update. stored version : %d, new: %d\n",rewrite_vars->sdt_version,sdt->version_number);
	if(rewrite_vars->sdt_version!=-1)
	  log_message(MSG_INFO,"The SDT version changed, so the channels names changed probably.\n");
	return 1;
      }
  return 0;

}

/** @brief update the version using the dowloaded SDT*/
void update_sdt_version(rewrite_parameters_t *rewrite_vars)
{
  sdt_t       *sdt=(sdt_t*)(rewrite_vars->full_sdt->packet);
  if(rewrite_vars->sdt_version!=sdt->version_number)
    log_message(MSG_DEBUG,"SDT rewrite : New sdt version. Old : %d, new: %d\n",rewrite_vars->sdt_version,sdt->version_number);

  rewrite_vars->sdt_version=sdt->version_number;
}


/** @brief Main function for sdt rewriting 
 * The goal of this function is to make a new sdt with only the announement for the streamed channel
 * by default it contains all the channels of the transponder. For each channel descriptor this function will search
 * the pmt pid of the channel in the given pid list. if found it keeps it otherwise it drops.
 * At the end, a new CRC32 is computed. The buffer is overwritten, so the caller have to save it before.
 *
 * @param rewrite_vars the parameters for sdt rewriting
 * @param channels The array of channels
 * @param curr_channel the channel for wich we want to generate a SDT
 * @param buf : the received buffer, to get the TS header
 */
int sdt_channel_rewrite(rewrite_parameters_t *rewrite_vars, mumudvb_channel_t *channel,  unsigned char *buf, int curr_channel)
{
  ts_header_t *ts_header=(ts_header_t *)buf;
  sdt_t       *sdt=(sdt_t*)(rewrite_vars->full_sdt->packet);
  sdt_descr_t  *sdt_descr; //was prog
  unsigned long crc32;

  int found=0;
  //destination buffer
  unsigned char buf_dest[188];
  int buf_dest_pos=0;
  int buffer_pos=SDT_LEN;

  int section_length=0;
  int new_section_length;
  int descriptor_length;

  section_length=HILO(sdt->section_length);

  //lets start the copy
  //we copy the ts header and adapt it a bit
  //the continuity counter is updated elswhere
  if(ts_header->payload_unit_start_indicator)
  {
    log_message(MSG_DEBUG,"SDT rewrite : pointer field 0x%x \n", buf[TS_HEADER_LEN-1]);
  }
  ts_header->payload_unit_start_indicator=1;
  buf[TS_HEADER_LEN-1]=0;//we erase the pointer field
  memcpy(buf_dest,ts_header,TS_HEADER_LEN);
  //we copy the modified SDT header
  sdt->current_next_indicator=1; //applicable immediately
  sdt->section_number=0;         //only one sdt
  sdt->last_section_number=0;
  memcpy(buf_dest+TS_HEADER_LEN,sdt,SDT_LEN);

  buf_dest_pos=TS_HEADER_LEN+SDT_LEN;

  log_message(MSG_DEBUG,"SDT rewrite : table id 0x%x \n", sdt->table_id);
  if(sdt->table_id!=0x42)
  {
    rewrite_vars->sdt_needs_update=1;
    rewrite_vars->full_sdt->empty=1;
    log_message(MSG_DETAIL,"SDT rewrite :We didn't got the good SDT (wrong table id) we search for a new one\n");
    return 0;
  }

  //We copy what we need : EIT announce and present PMT announce
  //strict comparaison due to the calc of section len cf down
  while((buffer_pos+SDT_DESCR_LEN)<(section_length) && !found)
  {
    sdt_descr=(sdt_descr_t *)((char*)rewrite_vars->full_sdt->packet+buffer_pos);
    descriptor_length=HILO(sdt_descr->descriptors_loop_length);
      //We check the transport stream id if present and the size of the packet
      // + 4 for the CRC32
    if((buf_dest_pos+SDT_DESCR_LEN+descriptor_length+4<TS_PACKET_SIZE) &&
          (channel->ts_id == HILO(sdt_descr->service_id) ))
      {
        if(buf_dest_pos+SDT_DESCR_LEN+descriptor_length+4+1>TS_PACKET_SIZE) //The +4 is for CRC32 +1 is because indexing starts at 0
          {
            log_message(MSG_WARN,"SDT rewrite : The generated SDT is too big for channel %d : \"%s\"\n", curr_channel, channel->name);
          }
          else
          {
            log_message(MSG_DETAIL,"SDT rewrite : NEW program for channel %d : \"%s\". ts_id : %d\n", curr_channel, channel->name,channel->ts_id);
            //we found a announce for a program in our stream, we keep it
            memcpy(buf_dest+buf_dest_pos,rewrite_vars->full_sdt->packet+buffer_pos,SDT_DESCR_LEN+descriptor_length);
            buf_dest_pos+=SDT_DESCR_LEN+descriptor_length;
            found=1;
          }
      }
      else
        log_message(MSG_DEBUG,"SDT rewrite : Program dropped. channel %d :\"%s\". ts_id chan : %d ts_id prog %d\n", 
                    curr_channel,
                    channel->name,
                    channel->ts_id,
                    HILO(sdt_descr->service_id));
      buffer_pos+=SDT_DESCR_LEN+descriptor_length;
  }
 
  //we compute the new section length
  //section lenght is the size of the section after section_length (crc32 included : 4 bytes)
  //so it's size of the crc32 + size of the sdt descriptors + size of the sdt header - 3 first bytes (the sdt header until section length included)
  //Finally it's total_sdt_data_size + 1
  new_section_length=buf_dest_pos-TS_HEADER_LEN + 1;

  //We write the new section length
  buf_dest[1+TS_HEADER_LEN]=(((new_section_length)&0x0f00)>>8)  | (0xf0 & buf_dest[1+TS_HEADER_LEN]);
  buf_dest[2+TS_HEADER_LEN]=new_section_length & 0xff;


  //CRC32 calculation inspired by the xine project
  //Now we must adjust the CRC32
  //we compute the CRC32
  crc32=0xffffffff;
  int i;
  for(i = 0; i < new_section_length-1; i++) {
    crc32 = (crc32 << 8) ^ crc32_table[((crc32 >> 24) ^ buf_dest[i+TS_HEADER_LEN])&0xff];
  }


  //We write the CRC32 to the buffer
  /// @todo check if Is this one safe with little/big endian ?
  buf_dest[buf_dest_pos]=(crc32>>24) & 0xff;
  buf_dest_pos+=1;
  buf_dest[buf_dest_pos]=(crc32>>16) & 0xff;
  buf_dest_pos+=1;
  buf_dest[buf_dest_pos]=(crc32>>8) & 0xff;
  buf_dest_pos+=1;
  buf_dest[buf_dest_pos]=crc32 & 0xff;
  buf_dest_pos+=1;


  //Padding with 0xFF 
  memset(buf_dest+buf_dest_pos,0xFF,TS_PACKET_SIZE-buf_dest_pos);

  if(found)
  {
    //We copy the result to the intended buffer
    memcpy(channel->generated_sdt,buf_dest,TS_PACKET_SIZE);
    channel->sdt_rewrite_skip=0;
  }
  else
  {
    log_message(MSG_WARN,"SDT rewrite : The SDT rewrite failed (no program found, have you set ts_id ?) we desactivate for this channel %d : \"%s\"\n", curr_channel, channel->name);
    channel->sdt_rewrite_skip=1;
  }

  //Everything is Ok ....*/
  return 1;
}

/** @brief This function is called when a new SDT packet for all channels is there and we asked for rewrite
 * this function save the full SDT wich will be the source SDT for all the channels
 * @return return 1 when the packet is updated
 */
int sdt_rewrite_new_global_packet(unsigned char *ts_packet, rewrite_parameters_t *rewrite_vars)
{
  sdt_t       *sdt=NULL;
  /*Check the version before getting the full packet*/
  if(!rewrite_vars->sdt_needs_update)
  {
    rewrite_vars->sdt_needs_update=sdt_need_update(rewrite_vars,ts_packet);
    if(rewrite_vars->sdt_needs_update) //It needs update we mark the packet as empty
      rewrite_vars->full_sdt->empty=1;
  }
  /*We need to update the full packet, we download it*/
  if(rewrite_vars->sdt_needs_update)
  {
    if(get_ts_packet(ts_packet,rewrite_vars->full_sdt))
    {
      sdt=(sdt_t*)(rewrite_vars->full_sdt->packet);
      if(sdt->table_id!=0x42)
      {
        rewrite_vars->sdt_needs_update=1;
        rewrite_vars->full_sdt->empty=1;
        log_message(MSG_DETAIL,"SDT rewrite :We didn't got the good SDT (wrong table id) we search for a new one\n");
        return 0;
      }
      else
      {
	log_message(MSG_DETAIL,"SDT rewrite : Full sdt updated\n");
	/*We've got the FULL SDT packet*/
	update_sdt_version(rewrite_vars);
	rewrite_vars->sdt_needs_update=0;
	rewrite_vars->full_sdt_ok=1;
	return 1;
       }
    }
  }
  //To avoid the duplicates, we have to update the continuity counter
  rewrite_vars->sdt_continuity_counter++;
  rewrite_vars->sdt_continuity_counter= rewrite_vars->sdt_continuity_counter % 32;
  return 0;
}


/** @brief This function is called when a new SDT packet for a channel is there and we asked for rewrite
 * This function copy the rewritten SDT to the buffer. And checks if the SDT was changed so the rewritten version have to be updated
*/
int sdt_rewrite_new_channel_packet(unsigned char *ts_packet, rewrite_parameters_t *rewrite_vars, mumudvb_channel_t *channel, int curr_channel)
{
  if(rewrite_vars->full_sdt_ok ) //the global full sdt is ok
  {
    /*We check if it's the first sdt packet ? or we send it each time ?*/
    /*We check if the versions corresponds*/
    if(!rewrite_vars->sdt_needs_update && channel->generated_sdt_version!=rewrite_vars->sdt_version)//We check the version only if the SDT is not currently updated
    {
      log_message(MSG_DEBUG,"SDT rewrite : We need to rewrite the SDT for the channel %d : \"%s\"\n", curr_channel, channel->name);
      /*They mismatch*/
      /*We generate the rewritten packet*/
      if(sdt_channel_rewrite(rewrite_vars, channel, ts_packet, curr_channel))
      {
        /*We update the version*/
        channel->generated_sdt_version=rewrite_vars->sdt_version;
      }
      else
      {
        log_message(MSG_DEBUG,"SDT rewrite : ERROR with the sdt for the channel %d : \"%s\"\n", curr_channel, channel->name);
        return 0;
      }

    }
    if(channel->generated_sdt_version==rewrite_vars->sdt_version)
    {
      /*We send the rewrited SDT from channel->generated_sdt*/
      memcpy(ts_packet,channel->generated_sdt,TS_PACKET_SIZE);
      //To avoid the duplicates, we have to update the continuity counter
      set_continuity_counter(ts_packet,rewrite_vars->sdt_continuity_counter);
    }
    else
    {
      return 0;
      log_message(MSG_DEBUG,"SDT rewrite : Bad sdt channel version, we don't send the sdt for the channel %d : \"%s\"\n", curr_channel, channel->name);
    }
  }
  else
  {
    return 0;
    log_message(MSG_DEBUG,"SDT rewrite : We need a global sdt update, we don't send the sdt for the channel %d : \"%s\"\n", curr_channel, channel->name);
  }
  return 1;

}


