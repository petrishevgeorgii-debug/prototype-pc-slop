/* Exercise actual guest file operations on a linked overlay and writable saves. */
#include "../src/runtime_file.c"
#include <assert.h>
#include <stdlib.h>
static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
int main(void) {
    char root[]="/tmp/port-mod-files-XXXXXX";
    assert(mkdtemp(root));
    char game[512],user[512],source[512],link[512];
    snprintf(game,sizeof(game),"%s/game",root);
    snprintf(user,sizeof(user),"%s/user",root);
    snprintf(source,sizeof(source),"%s/mod.dcx",root);
    snprintf(link,sizeof(link),"%s/game/asset.dcx",root);
    assert(!mkdir(game,0755));
    FILE *f=fopen(source,"w"); assert(f); assert(fputs("modded",f)>=0); assert(!fclose(f));
    assert(!symlink(source,link));
    runtime_file_configure(game,user);
    /* THEORDER_FILE_OVERRIDES: a generated file (lower-case path) replaces the game's. */
    char overrides[512], ov_dir[600], ov_file[700], game_dir[600], game_file[700];
    snprintf(overrides,sizeof(overrides),"%s/ov",root);
    snprintf(ov_dir,sizeof(ov_dir),"%s/hud",overrides);
    snprintf(ov_file,sizeof(ov_file),"%s/text.rz",ov_dir);
    snprintf(game_dir,sizeof(game_dir),"%s/HUD",game);
    snprintf(game_file,sizeof(game_file),"%s/Text.rz",game_dir);
    assert(!mkdir(overrides,0755) && !mkdir(ov_dir,0755) && !mkdir(game_dir,0755));
    f=fopen(ov_file,"w"); assert(f); assert(fputs("generated",f)>=0); assert(!fclose(f));
    f=fopen(game_file,"w"); assert(f); assert(fputs("orig",f)>=0); assert(!fclose(f));
    setenv("THEORDER_FILE_OVERRIDES",overrides,1);
    int ov=(int)do_open("/app0/HUD/Text.rz",0,0); assert(ov>=3);
    char ov_content[16]={0}; assert(do_read(ov,ov_content,9)==9 && !strcmp(ov_content,"generated"));
    GuestStat ov_info; assert(!do_stat("/app0/HUD/Text.rz",&ov_info) && ov_info.size==9);
    assert(!do_close(ov));
    /* An archive with a .patch: two bytes replaced, four added past its end. */
    char arc[700], patch[700];
    snprintf(arc,sizeof(arc),"%s/arc.rcf",game);
    snprintf(patch,sizeof(patch),"%s/arc.rcf.patch",overrides);
    f=fopen(arc,"wb"); assert(f); assert(fwrite("0123456789",1,10,f)==10); assert(!fclose(f));
    f=fopen(patch,"wb"); assert(f);
    const uint64_t head[3]={10,16,0}; /* base size, new size, count (u32) + pad */
    uint64_t counted[3]; memcpy(counted,head,sizeof(head)); counted[2]=2;
    assert(fwrite("G3PATCH1",1,8,f)==8 && fwrite(counted,8,3,f)==3);
    const uint64_t regions[6]={2,2,80, 12,4,82};
    assert(fwrite(regions,8,6,f)==6 && fwrite("ABWXYZ",1,6,f)==6 && !fclose(f));
    int pf=(int)do_open("/app0/arc.rcf",0,0); assert(pf>=3);
    char got[32]={0};
    assert(do_read(pf,got,32)==16 && !memcmp(got,"01AB456789\0\0WXYZ",16));
    assert(do_lseek(pf,-4,2)==12);
    memset(got,0,sizeof(got)); assert(do_read(pf,got,8)==4 && !memcmp(got,"WXYZ",4));
    memset(got,0,sizeof(got)); assert(do_pread(pf,got,3,1)==3 && !memcmp(got,"1AB",3));
    GuestStat ps; assert(!do_fstat(pf,&ps) && ps.size==16);
    assert(!do_stat("/app0/arc.rcf",&ps) && ps.size==16);
    assert(!do_close(pf));
    assert(!unlink(patch) && !unlink(arc));
    assert(!unlink(ov_file) && !rmdir(ov_dir) && !rmdir(overrides) && !unlink(game_file) && !rmdir(game_dir));
    for (int i=0;i<3;++i) {
        const char *path=i==0 ? "/app0/asset.dcx" : i==1 ? "/hostapp/asset.dcx" : "asset.dcx";
        int fd=(int)do_open(path,0,0); assert(fd>=3);
        char content[8]={0}; assert(do_read(fd,content,6)==6 && !strcmp(content,"modded"));
        GuestStat info; assert(!do_stat(path,&info) && info.size==6);
        assert(!do_close(fd));
        assert(do_open(path,2,0)==-EROFS);
        assert(do_open(path,0x400,0)==-EROFS);
        assert(do_truncate(path,0)==-EROFS);
        assert(path_op(path,2,0)==-EROFS);
        assert(do_rename(path,"/data/moved")==-EROFS);
    }
    /* An exact-file mount replaces one asset; its siblings under /app0 still come from the game. */
    char replacement[512], other[512];
    snprintf(replacement,sizeof(replacement),"%s/replacement.dcx",root);
    snprintf(other,sizeof(other),"%s/game/other.dcx",root);
    f=fopen(replacement,"w"); assert(f); assert(fputs("exact",f)>=0); assert(!fclose(f));
    f=fopen(other,"w"); assert(f); assert(fputs("plain",f)>=0); assert(!fclose(f));
    assert(!runtime_file_mount("/app0/asset.dcx",replacement));
    int exact=(int)do_open("/app0/asset.dcx",0,0); assert(exact>=3);
    char exact_content[8]={0}; assert(do_read(exact,exact_content,5)==5 && !strcmp(exact_content,"exact"));
    GuestStat exact_info; assert(!do_stat("/app0/asset.dcx",&exact_info) && exact_info.size==5);
    assert(!do_close(exact));
    assert(do_open("/app0/asset.dcx",2,0)==-EROFS);
    int sibling=(int)do_open("/app0/other.dcx",0,0); assert(sibling>=3);
    char sibling_content[8]={0}; assert(do_read(sibling,sibling_content,5)==5 && !strcmp(sibling_content,"plain"));
    assert(!do_close(sibling));
    runtime_file_unmount("/app0/asset.dcx");
    int dir=(int)do_open("/app0",0x20000,0); assert(dir>=3);
    char entries[1024]; int64_t count=do_getdents(dir,entries,sizeof(entries),NULL); assert(count>0);
    int found=0;
    for (int64_t p=0;p<count;) {
        uint16_t length; memcpy(&length,entries+p+4,2);
        assert(length);
        if (!strcmp(entries+p+8,"asset.dcx")) { assert(entries[p+6]==8); found=1; }
        p+=length;
    }
    assert(found && !do_close(dir));
    int save=(int)do_open("/data/test-save",0x202,0644); assert(save>=3);
    assert(do_write(save,"save",4)==4 && !do_close(save));
    assert(!path_op("/data/test-save",2,0));
    assert(!unlink(link) && !unlink(other) && !unlink(source) && !unlink(replacement) && !rmdir(game));
    const char *dirs[]={"temp0","download0","data"};
    for (int i=0;i<3;++i) { char p[1024]; snprintf(p,sizeof(p),"%s/%s",user,dirs[i]); assert(!rmdir(p)); }
    assert(!rmdir(user) && !rmdir(root));
    puts("Guest mod files: reads, stat, merged listing, readonly assets and writable saves PASS");
}
