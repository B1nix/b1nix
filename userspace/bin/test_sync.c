#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

int main() {
    printf("Testing sync and fsync...\n");
    
    int fd = open("/test.txt", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    
    const char *data = "Hello, durability!\n";
    if (write(fd, data, strlen(data)) < 0) {
        perror("write");
        close(fd);
        return 1;
    }
    
    printf("Data written, calling fsync...\n");
    if (fsync(fd) < 0) {
        perror("fsync");
        close(fd);
        return 1;
    }
    
    printf("fsync successful, calling sync...\n");
    sync();
    
    close(fd);
    printf("Test completed successfully.\n");
    return 0;
}
