#include <cassert>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <iterator>
#include <algorithm>
#include <syslog.h>
#include <sys/wait.h>
#include <autosprintf.h>
#include "eyekinfig.h"
#include "eyetil.h"
#include "soapeyefiService.h"
#ifdef HAVE_SQLITE
# include "iiidb.h"
#endif

static binary_t session_nonce;
#ifdef HAVE_SQLITE
    static struct {
	std::string filesignature;
	long filesize;
	std::string filename;
	inline void reset() { filesignature.erase(); filename.erase(); filesize=0; }
	inline void set(const std::string n,const std::string sig,long siz) {
	    filename = n; filesignature = sig; filesize = siz;
	}
	inline bool is(const std::string n,const std::string sig,long siz) {
	    return filesize==siz && filename==n && filesignature==sig;
	}
    }   already;
#endif /* HAVE_SQLITE */

static bool detached_child() {
    pid_t p = fork();
    if(p<0) {
	syslog(LOG_ERR,"Failed to fork away for hook execution");
	_exit(-1);
    }
    if(!p) {
	setsid();
	for(int i=getdtablesize();i>=0;--i) close(i);
	int i=open("/dev/null",O_RDWR); assert(i==0);
	i = dup(i); assert(i==1);
	i = dup(i); assert(i==2);
	return true;
    }
    return false;
}

static int E(eyefiService* efs,const char *c,const std::exception& e) {
    efs->keep_alive=0;
    syslog(LOG_ERR,"error while processing %s: %s",c,e.what());
    return soap_sender_fault(efs,gnu::autosprintf("error processing %s",c),0);
}

int eyefiService::StartSession(
	std::string macaddress,std::string cnonce,
	int transfermode,long transfermodetimestamp,
	struct rns__StartSessionResponse &r ) try {
    syslog(LOG_INFO,
	    "StartSession request from %s with cnonce=%s, transfermode=%d, transfermodetimestamp=%ld",
	    macaddress.c_str(), cnonce.c_str(), transfermode, transfermodetimestamp );
    eyekinfig_t eyekinfig(macaddress);
    r.credential = binary_t(macaddress+cnonce+eyekinfig.get_upload_key()).md5().hex();

    r.snonce = session_nonce.make_nonce().hex();
    r.transfermode=transfermode;
    r.transfermodetimestamp=transfermodetimestamp;
    r.upsyncallowed=false;

    std::string cmd = eyekinfig.get_on_start_session();
    if(!cmd.empty()) {
	if(detached_child()) {
	    putenv( gnu::autosprintf("EYEFI_MACADDRESS=%s",macaddress.c_str()) );
	    putenv( gnu::autosprintf("EYEFI_TRANSFERMODE=%d",transfermode) );
	    putenv( gnu::autosprintf("EYEFI_TRANSFERMODETIMESTAMP=%ld",transfermodetimestamp) );
	    char *argv[] = { (char*)"/bin/sh", (char*)"-c", (char*)cmd.c_str(), 0 };
	    execv("/bin/sh",argv);
	    syslog(LOG_ERR,"Failed to execute '%s'",cmd.c_str());
	    _exit(-1);
	}
    }
    return SOAP_OK;
}catch(const std::exception& e) { return E(this,"StartSession",e); }

int eyefiService::GetPhotoStatus(
	std::string credential, std::string macaddress,
	std::string filename, long filesize, std::string filesignature,
	int flags,
	struct rns__GetPhotoStatusResponse &r ) try {
    syslog(LOG_INFO,
	    "GetPhotoStatus request from %s with credential=%s, filename=%s, filesize=%ld, filesignature=%s, flags=%d; session nonce=%s",
	    macaddress.c_str(), credential.c_str(), filename.c_str(), filesize, filesignature.c_str(), flags,
	    session_nonce.hex().c_str() );

    eyekinfig_t eyekinfig(macaddress);
    std::string computed_credential = binary_t(macaddress+eyekinfig.get_upload_key()+session_nonce.hex()).md5().hex();

#ifndef NDEBUG
    syslog(LOG_DEBUG, " computed credential=%s", computed_credential.c_str());
#endif

    if (credential != computed_credential) throw std::runtime_error("card authentication failed");

#ifdef HAVE_SQLITE
    iiidb_t D(eyekinfig);
    seclude::stmt_t S = D.prepare(
	    "SELECT fileid FROM photo"
	    " WHERE mac=:mac AND filename=:filename"
	    "  AND filesize=:filesize AND filesignature=:filesignature"
    ).bind(":mac",macaddress)
     .bind(":filename",filename).bind(":filesize",filesize)
     .bind(":filesignature",filesignature);
    if(!S.step()) {
	r.fileid = 1; r.offset = 0;
    }else{
	r.fileid = S.column<long>(0);
	r.offset = filesize;
	already.set(filename,filesignature,filesize);
    }
#else /* HAVE_SQLITE */
    r.fileid=1, r.offset=0;
#endif /* HAVE_SQLITE */
    return SOAP_OK;
}catch(const std::exception& e) { return E(this,"GetPhotoStatus",e); }

int eyefiService::MarkLastPhotoInRoll(
	std::string macaddress, int mergedelta,
	struct rns__MarkLastPhotoInRollResponse&/* r */ ) try {
    syslog(LOG_INFO,
	    "MarkLastPhotoInRoll request from %s with mergedelta=%d",
	    macaddress.c_str(), mergedelta );
    std::string cmd = eyekinfig_t(macaddress).get_on_mark_last_photo_in_roll();
    if(!cmd.empty()) {
	if(detached_child()) {
	    putenv( gnu::autosprintf("EYEFI_MACADDRESS=%s",macaddress.c_str()) );
	    putenv( gnu::autosprintf("EYEFI_MERGEDELTA=%d",mergedelta) );
	    char *argv[] = { (char*)"/bin/sh", (char*)"-c", (char*)cmd.c_str(), 0 };
	    execv("/bin/sh",argv);
	    syslog(LOG_ERR,"Failed to execute '%s'",cmd.c_str());
	    _exit(-1);
	}
    }
    keep_alive = 0;
    return SOAP_OK;
}catch(const std::exception& e) { return E(this,"MarkLastPhotoInRoll",e); }

int eyefiService::UploadPhoto(
	int fileid, std::string macaddress,
	std::string filename, long filesize, std::string filesignature,
	std::string encryption, int flags,
	struct rns__UploadPhotoResponse& r ) try {
    syslog(LOG_INFO,
	    "UploadPhoto request from %s with fileid=%d, filename=%s, filesize=%ld,"
	    " filesignature=%s, encryption=%s, flags=%04X",
	    macaddress.c_str(), fileid, filename.c_str(), filesize,
	    filesignature.c_str(), encryption.c_str(), flags );
    std::string::size_type fnl=filename.length();
    if(fnl<sizeof(".tar") || strncmp(filename.c_str()+fnl-sizeof(".tar")+sizeof(""),".tar",sizeof(".tar")))
	throw std::runtime_error(gnu::autosprintf("honestly, I expected the tarball coming here, not '%s'",filename.c_str()));
    std::string the_file(filename,0,fnl-sizeof(".tar")+sizeof(""));
    std::string the_log = the_file+".log";

    eyekinfig_t eyekinfig(macaddress);

    umask(eyekinfig.get_umask());

    std::string td = eyekinfig.get_targetdir();
    tmpdir_t indir(td+"/.incoming.XXXXXX");

    std::string tf,lf;
    binary_t digest, idigest;
#ifdef HAVE_SQLITE
    bool beenthere = false;
#endif

    for(soap_multipart::iterator i=mime.begin(),ie=mime.end();i!=ie;++i) {
#ifndef NDEBUG
	syslog(LOG_DEBUG,
		" MIME attachment with id=%s, type=%s, size=%ld",
		(*i).id, (*i).type, (long)(*i).size );
#endif

	if((*i).id && !strcmp((*i).id,"INTEGRITYDIGEST")) {
	    std::string idigestr((*i).ptr,(*i).size);
#ifndef NDEBUG
	    syslog(LOG_DEBUG, " INTEGRITYDIGEST=%s", idigestr.c_str());
#endif
	    idigest.from_hex(idigestr);
	}
	if( (*i).id && !strcmp((*i).id,"FILENAME") ) {
	    assert( (*i).type && !strcmp((*i).type,"application/x-tar") );
#ifdef III_SAVE_TARS
	    std::string tarfile = indir.get_file(filename);
	    {
		std::ofstream(tarfile.c_str(),std::ios::out|std::ios::binary).write((*i).ptr,(*i).size);
	    }
#endif

	    if(!tf.empty()) throw std::runtime_error("already seen tarball");
	    if(!digest.empty()) throw std::runtime_error("already have integrity digest");
	    digest = integrity_digest((*i).ptr,(*i).size,eyekinfig.get_upload_key());
#ifndef NDEBUG
	    syslog(LOG_DEBUG," computed integrity digest=%s", digest.hex().c_str());
#endif
#ifdef HAVE_SQLITE
	    if(!(*i).size) {
		if(!already.is(filename,filesignature,filesize))
		    throw std::runtime_error("got zero-length upload for unknown file");
		beenthere = true; continue;
	    }
#endif

	    tarchive_t a((*i).ptr,(*i).size);
	    while(a.read_next_header()) {
		std::string ep = a.entry_pathname(), f = indir.get_file(ep);
		if(ep==the_file) tf = f;
		else if(ep==the_log) lf = f;
		else continue;
		int fd=open(f.c_str(),O_CREAT|O_WRONLY,0666);
		if(fd<0)
		    throw std::runtime_error(gnu::autosprintf("failed to create output file '%s'",f.c_str()));
		if(!a.read_data_into_fd(fd))
		    throw std::runtime_error(gnu::autosprintf("failed to untar file into '%s'",f.c_str()));
		close(fd);
	    }
	}
    }

#ifdef HAVE_SQLITE
    if(beenthere) {
	r.success=true;
	return SOAP_OK;
    }
#endif

    if(tf.empty()) throw std::runtime_error("haven't seen THE file");
    if(digest!=idigest) throw std::runtime_error("integrity digest verification failed");

    std::string::size_type ls = tf.rfind('/');
    // XXX: actually, lack of '/' signifies error here
    std::string tbn = (ls==std::string::npos)?tf:tf.substr(ls+1);
    ls = lf.rfind('/');
    std::string lbn = (ls==std::string::npos)?lf:lf.substr(ls+1);
    std::string ttf,tlf;
    bool success = false;
    for(int i=0;i<32767;++i) {
	const char *fmt = i ? "%1$s/(%3$05d)%2$s" : "%1$s/%2$s";
	ttf = (const char*)gnu::autosprintf(fmt,td.c_str(),tbn.c_str(),i);
	if(!lf.empty()) tlf = (const char*)gnu::autosprintf(fmt,td.c_str(),lbn.c_str(),i);
	if( (!link(tf.c_str(),ttf.c_str())) && (lf.empty() || !link(lf.c_str(),tlf.c_str())) ) {
	    unlink(tf.c_str());
	    if(!lf.empty()) unlink(lf.c_str());
	    success=true;
	    break;
	}
    }
    std::string cmd = eyekinfig.get_on_upload_photo();
    if(success) {
#ifdef HAVE_SQLITE
	{
	    iiidb_t D(eyekinfig);
	    D.prepare(
		    "INSERT INTO photo"
		    " (ctime,mac,fileid,filename,filesize,filesignature,encryption,flags)"
		    " VALUES"
		    " (:ctime,:mac,:fileid,:filename,:filesize,:filesignature,:encryption,:flags)"
	    ).bind(":ctime",time(0))
	     .bind(":mac",macaddress)
	     .bind(":fileid",fileid).bind(":filename",filename)
	     .bind(":filesize",filesize).bind(":filesignature",filesignature)
	     .bind(":encryption",encryption).bind(":flags",flags)
	     .step();
	}
#endif /* HAVE_SQLITE */
	if((!cmd.empty()) && detached_child()) {
	    putenv( gnu::autosprintf("EYEFI_UPLOADED_ORIG=%s",tbn.c_str()) );
	    putenv( gnu::autosprintf("EYEFI_MACADDRESS=%s",macaddress.c_str()) );
	    putenv( gnu::autosprintf("EYEFI_UPLOADED=%s",ttf.c_str()) );
	    if(!lf.empty()) putenv( gnu::autosprintf("EYEFI_LOG=%s",tlf.c_str()) );
	    char *argv[] = { (char*)"/bin/sh", (char*)"-c", (char*)cmd.c_str(), 0 };
	    execv("/bin/sh",argv);
	    syslog(LOG_ERR,"Failed to execute '%s'",cmd.c_str());
	    _exit(-1);
	}
    }

    r.success = true;
    return SOAP_OK;
}catch(const std::exception& e) { return E(this,"UploadPhoto",e); }
