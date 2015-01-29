/*
 *  V4L2 video capture example
 *
 *  This program can be used and distributed without restrictions.
 *
 *      This program is provided with the V4L2 API
 * see http://linuxtv.org/docs.php for more information
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <getopt.h>             /* getopt_long() */

#include <fcntl.h>              /* low-level i/o */
//#include <unistd.h>
//#include <errno.h>
#include <sys/stat.h>
//#include <sys/types.h>
//#include <sys/time.h>
//#include <sys/mman.h>
//#include <sys/ioctl.h>

#include <linux/videodev2.h>

int main(int argc, char **argv) {
	char* szDeviceID = "/dev/video0";
	int fd_cam = 0;

/* 디바이스 파일 열기 */
	fd_cam = open(szDeviceID, O_RDWR | O_NONBLOCK);
	if(fd_cam < 0) {
		perror(sprintf("Can not open %s", szDeviceID));
		exit(EXIT_FAILURE);
	}
	printf("[%s] OPEN\n", szDeviceID);

/* 디바이스 제원 확인 */
//	__u8	driver[16];		/* i.e. "bttv" */
//	__u8	card[32];		/* i.e. "Hauppauge WinTV" */
//	__u8	bus_info[32];	/* "PCI:" + pci_name(pci_dev) */
//	__u32   version;        /* should use KERNEL_VERSION() */
//	__u32	capabilities;	/* Device capabilities */
//	__u32	reserved[4];

	struct v4l2_capability v4l2_cap;
	if ( -1 == ioctl(fd_cam, VIDIOC_QUERYCAP, &v4l2_cap)) {
		perror(sprintf("[%s] VIDIOC_QUERYCAP", szDeviceID));
		exit(EXIT_FAILURE);
	}
	printf("DRIVER NAME : %s\n", v4l2_cap.driver);
	printf("CARD NAME   : %s\n", v4l2_cap.card);
	printf("BUS         : %s\n", v4l2_cap.bus_info);
	printf("VERSION     : %d\n", v4l2_cap.version);


/* 포트 번호 획득 */
	struct v4l2_input input;
	int nPortIndex;
	if ( -1 == ioctl(fd_cam, VIDIOC_G_INPUT, &nPortIndex)) {
		perror(sprintf("[%s] VIDIOC_G_INPUT", szDeviceID));
		exit(EXIT_FAILURE);
	}

	memset(&input, 0x00, sizeof(input));
	input.index = nPortIndex;
	if( -1 == ioctl(fd_cam, VIDIOC_ENUMINPUT, &input)) {
		perror(sprintf("[%s] VIDIOC_ENUMINPUT", szDeviceID));
		exit(EXIT_FAILURE);
	}
	printf("[%s] NAME : %s ", szDeviceID, input.name);

/* 카메라 디바이스 확인 */
	switch(input.type) {
	case V4L2_INPUT_TYPE_TUNER :
		printf("is a tunner... damn.\n");
		exit(EXIT_FAILURE);
		break;
	case V4L2_INPUT_TYPE_CAMERA :
		printf("is a camera.\n");
		break;
	default :
		printf("is... what the fuck!?\n");
		exit(EXIT_FAILURE);
		break;
	}

/* 초기화 */


	printf("Covers 0x%08x\n", input.std);


	close(fd_cam);
}
