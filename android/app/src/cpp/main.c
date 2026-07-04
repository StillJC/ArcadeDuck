#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <getopt.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>
#include <pthread.h>

#define UART_DEV_NAME    "/dev/ttyUSB0"    // Serial device

volatile int lTrackBallDirection[4] = {0,0,0,0};//UP/DOWN/LEFT/RIGHT
volatile int Trackball_x=0;     // A better way to do it might be with absolute co-ordinates
volatile int Trackball_y=0;

//---------------------------------------------------------

/**Set the serial port parameters: baud rate, data bit, stop bit and validation bit
 * @param[in]  fd         Type  int      open serial file handle
 * @param[in]  nSpeed     Type  int     baud rate
 * @param[in]  nBits     Type  int     data bit value is 7 or 8
 * @param[in]  nParity     Type  int     stop bit value is 1 or 2
 * @param[in]  nStop      Type  int      validation type value is N,E,O,,S
 * @return     Return the setting result
 * - 0         successfully
 * - -1        failed
 **/
int UART_SetOpt(int fd, int nSpeed, int nBits, int nParity, int nStop)
{
    struct termios newtio, oldtio;

    //Save and test the existing serial port parameter settings. If there is an error in the serial port number, there will be related error messages.
    if (tcgetattr(fd, &oldtio) != 0)
    {
        perror("SetupSerial 1");
        return -1;
    }

    bzero(&newtio, sizeof(newtio));        //New termios parameter cleared
    newtio.c_cflag |= CLOCAL | CREAD;    //CLOCAL--Ignore the modem control line, local connection, no modem control function, CREAD--enable receiving flag
    //Set the number of data bits
    newtio.c_cflag &= ~CSIZE;    //Clear data bit flag
    switch (nBits)
    {
        case 7:
            newtio.c_cflag |= CS7;
            break;
        case 8:
            newtio.c_cflag |= CS8;
            break;
        default:
            fprintf(stderr, "Unsupported data size\n");
            return -1;
    }
    //Set check digit
    switch (nParity)
    {
        case 'o':
        case 'O':                     //Odd parity
            newtio.c_cflag |= PARENB;
            newtio.c_cflag |= PARODD;
            newtio.c_iflag |= (INPCK | ISTRIP);
            break;
        case 'e':
        case 'E':                     //Even parity
            newtio.c_iflag |= (INPCK | ISTRIP);
            newtio.c_cflag |= PARENB;
            newtio.c_cflag &= ~PARODD;
            break;
        case 'n':
        case 'N':                    //No verification
            newtio.c_cflag &= ~PARENB;
            break;
        default:
            fprintf(stderr, "Unsupported parity\n");
            return -1;
    }
    //Set stop bit
    switch (nStop)
    {
        case 1:
            newtio.c_cflag &= ~CSTOPB;
            break;
        case 2:
            newtio.c_cflag |= CSTOPB;
            break;
        default:
            fprintf(stderr,"Unsupported stop bits\n");
            return -1;
    }
    //Set the baud rate 2400/4800/9600/19200/38400/57600/115200/230400
    switch (nSpeed)
    {
        case 2400:
            cfsetispeed(&newtio, B2400);
            cfsetospeed(&newtio, B2400);
            break;
        case 4800:
            cfsetispeed(&newtio, B4800);
            cfsetospeed(&newtio, B4800);
            break;
        case 9600:
            cfsetispeed(&newtio, B9600);
            cfsetospeed(&newtio, B9600);
            break;
        case 19200:
            cfsetispeed(&newtio, B19200);
            cfsetospeed(&newtio, B19200);
            break;
        case 38400:
            cfsetispeed(&newtio, B38400);
            cfsetospeed(&newtio, B38400);
            break;
        case 57600:
            cfsetispeed(&newtio, B57600);
            cfsetospeed(&newtio, B57600);
            break;
        case 115200:
            cfsetispeed(&newtio, B115200);
            cfsetospeed(&newtio, B115200);
            break;
        case 230400:
            cfsetispeed(&newtio, B230400);
            cfsetospeed(&newtio, B230400);
            break;
        default:
            printf("\tSorry, Unsupported baud rate, set default 9600!\n\n");
            cfsetispeed(&newtio, B9600);
            cfsetospeed(&newtio, B9600);
            break;
    }
    //Set the minimum number of bytes and timeout for read
    newtio.c_cc[VTIME] = 1;     //Read a character and wait for 1*(1/10)s
    newtio.c_cc[VMIN] = 1;      //The minimum number of characters to read is 1

    tcflush(fd,TCIFLUSH);         //Empty the buffer
    if (tcsetattr(fd, TCSANOW, &newtio) != 0)    //Activate new settings
    {
        perror("SetupSerial 3");
        return -1;
    }

    printf("Serial set OK!\n");

    return 0;
}

/**Serial port read function
  * @param[in] fd           The file handle of the serial port
  * @param[in] *rcv_buf     Receive buffer pointer
  * @param[in] data_len     The length of the data to be read
  * @param[in] timeout      Receive wait timeout time, unit ms
  * @return returns the setting result
  *->0 set successfully
  *-Other Read timeout or error
 */
int UART_Recv(int fd, char *rcv_buff, int data_len, int timeout/*ms*/)
{
    fd_set fs_read;
    struct timeval time;

    time.tv_sec = timeout / 1000;
    time.tv_usec = timeout * 1000;

    FD_ZERO(&fs_read);        //The collection must be cleared every time the loop is repeated, otherwise the descriptor changes cannot be detected
    FD_SET(fd, &fs_read);    //Add descriptor

    int lLen, lSelRet;
    //Timeout waiting for read change, >0: positive number of ready description words, -1: error, 0: timeout
    lSelRet = select(fd + 1, &fs_read, NULL, NULL, &time);
    if(lSelRet > 0)
    {
#if 0
        if (0 != FD_ISSET(fd, &fs_read)){
        }
#endif
        lLen = read(fd, rcv_buff, data_len);
        return lLen;
    }
    else if(0 == lSelRet){
        //printf("TIMEOUT\n");
        return 0;
    }
    else{
        printf("Sorry, UART recv wrong!");
        return -1;
    }
}

/**Serial port sending function
  * @param[in] fd           The file handle of the serial port
  * @param[in] *send_buf    Send data pointer
  * @param[in] data_len     Send data length
  * @return return result
  *-data_len succeeded
  *--1 failed
 */
int UART_Send(int fd, char *send_buff, int data_len)
{
    ssize_t ret = 0;

    ret = write(fd, send_buff, data_len);
    if (ret == data_len)
    {
        //printf("send data is %s\n", send_buff);
        return ret;
    }
    else
    {
        printf("write device error\n");
        tcflush(fd,TCOFLUSH);
        return -1;
    }
}
#include <pthread.h>
static pthread_t gTrackBallID;

#define HARD_KEY_UP 0x1
#define HARD_KEY_DOWN 0x2
#define HARD_KEY_LEFT 0x4
#define HARD_KEY_RIGHT 0x8
unsigned short gTrackBallKeys = 0x0;
void *TrackBallThread(void *ptr){

    int fdSerial = -1;

    gTrackBallKeys = 0x0;

    //Turn on the serial device
    fdSerial = open(UART_DEV_NAME, O_RDWR | O_NOCTTY | O_NDELAY);
    if(fdSerial < 0)
    {
        perror(UART_DEV_NAME);
        return NULL;
    }
    //Set serial port blocking, 0: blocking, FNDELAY: non-blocking
    if (fcntl(fdSerial, F_SETFL, 0) < 0)    //Blocking, even if the previous setting in the open serial device is non-blocking
    {
        printf("UART fcntl F_SETFL failed!\n");
        close(fdSerial);
        fdSerial = -1;
        return NULL;
    }
    //Set serial port parameters
    if (UART_SetOpt(fdSerial, 115200, 8, 'N', 1)== -1)    //Set 8 data bits, 1 stop bit, no parity
    {
        printf("UART set opt Error\n");
        close(fdSerial);
        fdSerial = -1;
        return NULL;
    }

    //tcflush(fdSerial, TCIOFLUSH);    //Clear the serial buffer

    //-------------------------------------------------------------
    unsigned char lRecvBuff[1024];
    int lRecvLen = 0;

    int lLastRecvState = 1;

    #define DIRECTION_UP 0
    #define DIRECTION_DOWN 1
    #define DIRECTION_LEFT 2
    #define DIRECTION_RIGHT 3

    #define MINVALIDCNT 2
    #define MINNODELEN 4

    unsigned short lPrevLeftLen = 0;
    unsigned char lPrevLeftBytes[MINNODELEN];


    //Read data repeatedly
    while(1)
    {
        lRecvLen = UART_Recv(fdSerial, (char*)(lRecvBuff+MINNODELEN), 128, 16);
        if(lRecvLen > 0)
        {
#if 0
            for(int i=0;i<lRecvLen;i++){
                printf("%02x ",lRecvBuff[i+MINNODELEN] & 0xFF);
            }
            printf("\n");
#endif
            lLastRecvState = 1;
            //-----------------------------------------------------
            int lTotalLen = lRecvLen;
            int lStartPos = MINNODELEN;
            //Fill lPrevLeftBytes[]
            if(lPrevLeftLen < MINNODELEN){
                lTotalLen += lPrevLeftLen;
                lStartPos -= lPrevLeftLen;
                for(int i=0;i<lPrevLeftLen;i++){
                    lRecvBuff[i+lStartPos] = lPrevLeftBytes[i];
                }
            }
           //Adjust start pos(first byte must is 0xFF)
            int srcStartPos = lStartPos;
            int srcTotalLen = lTotalLen;
            for(int i=0;i<srcTotalLen;i++){
                if(0xFF == lRecvBuff[i+srcStartPos]){
                    break;
                }
                lStartPos++;
                lTotalLen--;
            }
            //New lPrevLeftBytes
            lPrevLeftLen = lTotalLen%MINNODELEN;
            lTotalLen -= lPrevLeftLen;
            if(lPrevLeftLen > 0){
                for(int i=0;i<lPrevLeftLen;i++){
                    lPrevLeftBytes[i] = lRecvBuff[lStartPos+lTotalLen+i];
                }
            }
            //transform UP/DOWN/LEFT/RIGHT
            for(int i=0;i<(lTotalLen/MINNODELEN);i++){
                unsigned short lData = 0;
                lData = (lRecvBuff[lStartPos+(i*MINNODELEN)+1] << 8) | lRecvBuff[lStartPos+(i*MINNODELEN)+2];
                switch(lData){
                    case 0x00FE://UP
                        lTrackBallDirection[DIRECTION_UP] += 1;
                        Trackball_x +=1;
                        break;
                    case 0x0001://DOWN
                        lTrackBallDirection[DIRECTION_DOWN] += 1;
                        Trackball_x -=1;
                        break;
                    case 0xFE00://LEFT
                        lTrackBallDirection[DIRECTION_LEFT] += 1;
                        Trackball_y -=1;
                        break;
                    case 0x0100://RIGHT
                        lTrackBallDirection[DIRECTION_RIGHT] += 1;
                        Trackball_y +=1;
                        break;
                    default:break;
                }
            }
            //printf("------------------------------------ UP=%d DOWN=%d LEFT=%d RIGHT=%d\n",lTrackBallDirection[DIRECTION_UP],lTrackBallDirection[DIRECTION_DOWN],lTrackBallDirection[DIRECTION_LEFT],lTrackBallDirection[DIRECTION_RIGHT]);
            unsigned short lTrackBallKeys = 0;
            //UP/DOWN
            if(lTrackBallDirection[DIRECTION_UP] >= MINVALIDCNT){
                lTrackBallKeys |= HARD_KEY_UP;
            }else if(lTrackBallDirection[DIRECTION_DOWN] >= MINVALIDCNT){
                lTrackBallKeys |= HARD_KEY_DOWN;
            }
            //LEFT/RIGHT
            if(lTrackBallDirection[DIRECTION_LEFT] >= MINVALIDCNT){
                lTrackBallKeys |= HARD_KEY_LEFT;
            }else if(lTrackBallDirection[DIRECTION_RIGHT] >= MINVALIDCNT){
                lTrackBallKeys |= HARD_KEY_RIGHT;
            }
            if(lTrackBallKeys != gTrackBallKeys){
                gTrackBallKeys = lTrackBallKeys;
            }
        }
        else if(0 == lRecvLen)
        {

            lPrevLeftLen = 0;
            for(int i=0;i<4;i++)
            {
                lTrackBallDirection[i] = 0;
            }
            gTrackBallKeys = 0;//new lock ???

            if(1 == lLastRecvState){
                lLastRecvState = 0;
                printf("TIMEOUT: Cannot receive data\n");
            }
        }
        else
        {
            lPrevLeftLen = 0;
            for(int i=0;i<4;i++){
                lTrackBallDirection[i] = 0;
            }
            gTrackBallKeys = 0;//need lock ???

            printf("Error: Receive data\n");
        }
    }

    gTrackBallKeys = 0x0;

    if(fdSerial > 0){
        close(fdSerial);
        fdSerial = -1;
    }

    pthread_exit((void *)0);
    return NULL;
}

int main (int argc, char *argv[])
{
    //TrackBall Thread
    int lRet = pthread_create(&gTrackBallID,NULL,TrackBallThread,NULL);
    if(lRet != 0){
        printf("Create TrackBall Thread Error!\n");
        return -1;
    }

    int lFrameCnt = 0;

    while (1)
    {
        printf ("%8.8X %8.8X\n", Trackball_x,Trackball_y);
        usleep (1000);
    }

}
