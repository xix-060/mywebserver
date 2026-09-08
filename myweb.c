#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<assert.h>
#include<string.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<fcntl.h>

int creat_socket()
{
    int sockfd=socket(AF_INET,SOCK_STREAM,0);
    if(sockfd==-1)
    {
        return -1;
    }

    struct sockaddr_in saddr;
    memset(&saddr,0,sizeof(saddr));

    saddr.sin_family=AF_INET;
    saddr.sin_port=htons(80);
    saddr.sin_addr.s_addr=inet_addr("127.0.0.1");

    int res=bind(sockfd,(struct sockaddr *)&saddr,sizeof(saddr));
    if(res==-1){
        return -1;
    }

    res=listen(sockfd,5);
    if(res==-1){
        return -1;
    }

    return sockfd;
}

int main()
{
    int sockfd=creat_socket();
    assert(sockfd!=-1);

    struct sockaddr_in caddr;
    int len=-1;
    int c=-1;

    while(1)
    {
        len=sizeof(caddr);

        c=accept(sockfd,(struct sockaddr *)&caddr,&len);
        if(c<0)
        {
            continue;
        }

        char buff[1024]={0};
        int n=recv(c,buff,1023,0);
        printf("n=%d,read:\n",n);
        printf("%s\n",buff);

        char *s=strtok(buff," ");
        if(s==NULL)
        {
            close(c);
            continue;
        }
        printf("请求方法：%s\n",s);

        s=strtok(NULL," ");
        if(s==NULL)
        {
            close(c);
            continue;
        }
        printf("请求的资源：%s\n",s);

        if(strcmp(s,"/")==0)
        {
            s="/index.html";
        }
        char path[128]={"/home/stu/quzijie/03_quzijie/test34"};
        strcat(path,s);

        int fd=open(path,O_RDONLY);
        if(fd==-1)
        {
            send(c,"404",3,0);
            close(c);
            continue;
        }

        int size=lseek(fd,0,SEEK_END);//求文件长度
        lseek(fd,0,SEEK_SET);

        //send(c,"welcome to us!",14,0);
        char sendbuff[512]={0};
        strcpy(sendbuff,"HTTP/1.1 200 OK\r\n");
        strcat(sendbuff,"Server:myweb_msx\r\n");
        //strcat(sendbuff,"Content-Length:14\r\n");
        sprintf(sendbuff+strlen(sendbuff),"Content-Length:%d\r\n",size);
        strcat(sendbuff,"\r\n");
        //strcat(sendbuff,"welcome to us!");

        printf("send:\n%s\n",sendbuff);
        send(c,sendbuff,strlen(sendbuff),0);
        char data[512]={0};
        int num=0;
        while((num=read(fd,data,512))>0)
        {
            send(c,data,num,0);
        }
        close(fd);
        close(c);//短链接
    }
    exit(0);
}
