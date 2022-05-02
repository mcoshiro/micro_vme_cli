//minimal utility for reading and writing to ethernet device
//29/04/2022 MO

#include <arpa/inet.h>        //for htons
#include <cstring>            //for strcpy, memcpy
#include <iostream>           //for cout
#include <fcntl.h>            //for open, read, write(?)
#include <net/if.h>           //for ifreq
#include <netinet/if_ether.h> //for ethhdr, ETH_ALEN(=6)
#include <stdexcept>          //for runtime_error
#include <stdio.h>            //for sscanf, printf
#include <sys/ioctl.h>        //for ioctl and associated constants/macros
#include <sys/socket.h>       //for socket
#include <unistd.h>           //for write, sleep

//utility to print <size> bytes from data at <data>
//TODO left fixing this
void print_hex(char* data, int size) {
  for (int i = 0; i < size; i++) {
    printf("%02hhx",data[i]);
    if (i % 16 == 15) {
      printf("\n");
    }
    else if (i % 2 == 1) {
      printf(" ");
    }
  }
  printf("\n");
}

int main(int argc, char** argv) {

  //general settings and constants
  const char vme_transfer_mode = 0x0; //single transfer mode (2 bits)
  const char vme_transfer_size = 0x1; //VME 16 bit data transfer (2 bits)
  const char vme_readwrite[2] = {0x0, 0x1}; //read, write respectively (1 bit)
  const char vme_address_mode = 0x2; //VME A24 address modifier (3 bits)
  //combine as (vme_transfer_mode|(vme_transfer_size<<2)
  //    |vme_readwrite[vme_iswrite]<<4|vme_address_mode<<5);
  bool debug = false;
  struct ethhdr ether_header;
  char dev_name[12] = "/dev/schar2"; //first LC fiber on network card
  //char dev_name[12] = "/dev/schar3"; //second LC fiber on network card
  char eth_name[7] = "ens3f0";
  unsigned char hw_dest_addr[6];
  unsigned int odmb_vme_slot = 15;
  char vcc_addr[18] = "02:00:00:00:00:4A";
  char msgbuf[100];
  int eth_header_size = sizeof(ether_header);

  //parse input to deduce VME commands and determine settings
  if (argc < 4) {
    std::cout << "ERROR: insufficient arguments\n";
    std::cout << "micro_vme_cli usage example:\n";
    std::cout << "./micro_vme_cli R 4100 0000\n";
    return -1;
  }
  //char str_vme_rw[2] = "R";
  //char str_vme_cmd[5] = "4100";
  //char str_vme_data[5] = "0000";
  unsigned short vme_cmd;
  unsigned short vme_data;
  unsigned char vme_iswrite = 0;
  if (strcmp(argv[1], "W")==0) {
    vme_iswrite = 1;
  }
  sscanf(argv[2],"%4hx",&vme_cmd);
  sscanf(argv[3],"%4hx",&vme_data);
  for (int arg_idx = 4; arg_idx < argc; arg_idx++) {
    if (strcmp(argv[arg_idx],"--debug")==0) {
      debug = true;
    }
    if (strcmp(argv[arg_idx],"--slot")==0) {
      sscanf(argv[arg_idx+1],"%2d",&odmb_vme_slot);
    }
  }
  unsigned int vme_addr = (vme_cmd & 0x07ffff) | (odmb_vme_slot << 19);

  //fill ethernet destination address (VCC)
  sscanf(vcc_addr,"%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
      hw_dest_addr,hw_dest_addr+1,hw_dest_addr+2,
      hw_dest_addr+3,hw_dest_addr+4,hw_dest_addr+5);
  std::memcpy(ether_header.h_dest, hw_dest_addr, ETH_ALEN);

  //open ethernet device
  int eth_device = open(dev_name, O_RDWR);
  if (eth_device == -1) {
    std::cout << "ERROR: unable to open device\n";
    throw std::runtime_error("Failed to open device");
  }

  //issue reset (_IO(0xbb, 0))
  if (ioctl(eth_device, _IO(0xbb, 0))==-1) {
    std::cout << "ERROR: unable to reset device\n";
    throw std::runtime_error("Failed to reset device");
  }

  //get source (this) ethernet (MAC) address
  struct ifreq mifr;
  std::strcpy(mifr.ifr_name, eth_name);
  int isock = socket(PF_INET, SOCK_STREAM, 0);
  if (isock == -1) {
    std::cout << "ERROR: unable to called socket\n";
    throw std::runtime_error("Failed to call socket");
  }
  if (ioctl(isock, SIOCGIFHWADDR, &mifr)==-1) {
    std::cout << "ERROR: unable to call SIOCGHWADDR on socket\n";
    throw std::runtime_error("Failed to call SIOCGHWADDR on socket");
  }
  std::memcpy(ether_header.h_source, mifr.ifr_addr.sa_data, ETH_ALEN);

  //build ethernet packet and send
  short payload_size = 10;
  if (vme_iswrite) payload_size += 2;
  ether_header.h_proto = htons(payload_size); //calculate payload size (bytes?)
  memcpy(msgbuf, &ether_header, eth_header_size); //header is 6+6+2=14 bytes
  msgbuf[eth_header_size] = 0x00;
  msgbuf[eth_header_size+1] = 0x20; //procs VME command
  msgbuf[eth_header_size+2] = 0x00; //nvme&0xff00>>8
  msgbuf[eth_header_size+3] = 0x00; //nvme&0x00ff
  msgbuf[eth_header_size+4] = 0x00;
  msgbuf[eth_header_size+5] = (vme_transfer_mode|(vme_transfer_size<<2)
      |vme_readwrite[vme_iswrite]<<4|vme_address_mode<<5);
  msgbuf[eth_header_size+6] = 0x00; 
  msgbuf[eth_header_size+7] = (vme_addr&0xff0000)>>16; 
  msgbuf[eth_header_size+8] = (vme_addr&0xff00)>>8; 
  msgbuf[eth_header_size+9] = (vme_addr&0xff)>>8; 
  msgbuf[eth_header_size+10] = (vme_data&0xff00)>>8; 
  msgbuf[eth_header_size+11] = (vme_data&0x00ff); 
  int msgsize = eth_header_size + payload_size;
  if (debug) print_hex(msgbuf, msgsize);
  write(eth_device, msgbuf, msgsize);

  //read from ethernet interface
  if (vme_iswrite == 0) {
    //int max_readsize = 2; //standard number of bytes to read??
    bool good_packet = 0;
    for (int i = 0; i < 10; i++) {
      int max_readsize = 1000;
      int readsize = read(eth_device, msgbuf, max_readsize);
      //bytes 0-5 are VCC addr, 6-11 are PC addr, 12-13 are ethertype (size?)
      //bytes 14-21 are VCC header, 15 is return type (should be 0x05)
      //bytes 22- are data
      if (debug) std::cout << "Read size: " << readsize << "\n";
      if (debug) print_hex(msgbuf, readsize);
      if (msgbuf[15] == 0x05) {
        good_packet = true;
        break;
      }
      printf("Bad packet, type %02hhx\n", msgbuf[15]);
      usleep(1000); //us
    }
    if (good_packet) {
      printf("VCC response: %02hhx%02hhx\n", msgbuf[22], msgbuf[23]);
    }
    else {
      std::cout << "VCC packet timeout.\n";
    }
  }

  return 0;
}
