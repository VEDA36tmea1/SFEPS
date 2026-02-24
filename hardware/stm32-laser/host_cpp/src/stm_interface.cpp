#include "stm_interface.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

namespace
{
int openSerial(const std::string& device, int baudrate)
{
    int fd = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0)
    {
        std::perror("openSerial: open failed");
        return -1;
    }

    termios tty{};
    if (tcgetattr(fd, &tty) != 0)
    {
        std::perror("openSerial: tcgetattr failed");
        ::close(fd);
        return -1;
    }

    cfmakeraw(&tty);

    speed_t speed;
    switch (baudrate)
    {
    case 115200: speed = B115200; break;
    case 57600:  speed = B57600;  break;
    case 38400:  speed = B38400;  break;
    case 19200:  speed = B19200;  break;
    case 9600:   speed = B9600;   break;
    default:     speed = B115200; break;
    }

    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 5; // 0.5s read timeout

    if (tcsetattr(fd, TCSANOW, &tty) != 0)
    {
        std::perror("openSerial: tcsetattr failed");
        ::close(fd);
        return -1;
    }

    return fd;
}

void writeAll(int fd, const char* buf, std::size_t len)
{
    std::size_t written = 0;
    while (written < len)
    {
        ssize_t n = ::write(fd, buf + written, len - written);
        if (n <= 0)
        {
            std::perror("writeAll: write failed");
            break;
        }
        written += static_cast<std::size_t>(n);
    }
}
} // namespace

StmInterface::StmInterface(const std::string& serial_device, int baudrate)
{
    fd_ = openSerial(serial_device, baudrate);
    if (fd_ < 0)
    {
        std::cerr << "StmInterface: failed to open serial " << serial_device << "\n";
    }
}

StmInterface::~StmInterface()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
    }
}

bool StmInterface::isOpen() const
{
    return fd_ >= 0;
}

void StmInterface::sendPwm(int pan_us, int tilt_us)
{
    if (!isOpen())
        return;

    char buf[64];
    int n = std::snprintf(buf, sizeof(buf), "%d %d\r\n", pan_us, tilt_us);
    if (n > 0)
    {
        writeAll(fd_, buf, static_cast<std::size_t>(n));
    }
}

void StmInterface::sendModeManual()
{
    if (!isOpen())
        return;

    const char* cmd = "mode 0\r\n";
    writeAll(fd_, cmd, std::strlen(cmd));
}

