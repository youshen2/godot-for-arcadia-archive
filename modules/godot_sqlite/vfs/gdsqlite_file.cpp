#include "gdsqlite_file.hpp"

using namespace godot;

/*
** Close a file.
*/
int gdsqlite_file::close(sqlite3_file *pFile) {
	gdsqlite_file *p = reinterpret_cast<gdsqlite_file *>(pFile);
	ERR_FAIL_COND_V(!p->file->is_open(), SQLITE_IOERR_CLOSE);

	p->file->close();
	p->file.unref();

	return SQLITE_OK;
}

/*
** Read data from a file.
*/
int gdsqlite_file::read(sqlite3_file *pFile, void *zBuf, int iAmt, sqlite_int64 iOfst) {
	gdsqlite_file *p = reinterpret_cast<gdsqlite_file *>(pFile);
	ERR_FAIL_COND_V(!p->file->is_open(), SQLITE_IOERR_CLOSE);
	ERR_FAIL_COND_V(iAmt < 0 || iOfst < 0, SQLITE_IOERR_READ);

	/* Seek the wanted position in the file */
	p->file->seek(iOfst);
	ERR_FAIL_COND_V(p->file->get_position() != iOfst, SQLITE_IOERR_READ);

	/* SQLite requires unread bytes to be zero-filled on a short read. */
	const uint64_t bytes_read = p->file->get_buffer(static_cast<uint8_t *>(zBuf), iAmt);
	if (bytes_read == uint64_t(iAmt)) {
		return SQLITE_OK;
	}
	memset(static_cast<uint8_t *>(zBuf) + bytes_read, 0, iAmt - bytes_read);
	return SQLITE_IOERR_SHORT_READ;
}

/*
** Write data to a file.
*/
int gdsqlite_file::write(sqlite3_file *pFile, const void *zBuf, int iAmt, sqlite_int64 iOfst) {
	gdsqlite_file *p = reinterpret_cast<gdsqlite_file *>(pFile);
	ERR_FAIL_COND_V(!p->file->is_open(), SQLITE_IOERR_CLOSE);
	ERR_FAIL_COND_V(iAmt < 0 || iOfst < 0, SQLITE_IOERR_WRITE);

	/* Seek the wanted position in the file */
	p->file->seek(iOfst);
	ERR_FAIL_COND_V(p->file->get_position() != iOfst, SQLITE_IOERR_READ);

	/* Write directly from SQLite's buffer to avoid an allocation and copy per page. */
	ERR_FAIL_COND_V(!p->file->store_buffer(static_cast<const uint8_t *>(zBuf), iAmt), SQLITE_IOERR_WRITE);

	/* Was the write succesful? */
	size_t bytes_written = p->file->get_position() - iOfst;
	ERR_FAIL_COND_V(bytes_written != iAmt, SQLITE_IOERR_WRITE);

	return SQLITE_OK;
}

/*
** Truncate a file. This is a no-op for this VFS.
*/
int gdsqlite_file::truncate(sqlite3_file *pFile, sqlite_int64 size) {
	gdsqlite_file *p = reinterpret_cast<gdsqlite_file *>(pFile);
	ERR_FAIL_COND_V(!p->file->is_open() || size < 0, SQLITE_IOERR_TRUNCATE);
	return p->file->resize(size) == OK ? SQLITE_OK : SQLITE_IOERR_TRUNCATE;
}

/*
** Sync the contents of the file to the persistent media.
*/
int gdsqlite_file::sync(sqlite3_file *pFile, int flags) {
	gdsqlite_file *p = reinterpret_cast<gdsqlite_file *>(pFile);
	ERR_FAIL_COND_V(!p->file->is_open(), SQLITE_IOERR_FSYNC);
	p->file->flush();
	return p->file->get_error() == OK ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

/*
** Write the size of the file in bytes to *pSize.
*/
int gdsqlite_file::fileSize(sqlite3_file *pFile, sqlite_int64 *pSize) {
	gdsqlite_file *p = reinterpret_cast<gdsqlite_file *>(pFile);
	ERR_FAIL_COND_V(!p->file->is_open(), SQLITE_IOERR_CLOSE);

	*pSize = p->file->get_length();

	return SQLITE_OK;
}

/*
** Locking functions. The xLock() and xUnlock() methods are both no-ops.
** The xCheckReservedLock() always indicates that no other process holds
** a reserved lock on the database file. This ensures that if a hot-journal
** file is found in the file-system it is rolled back.
*/
int gdsqlite_file::lock(sqlite3_file *pFile, int eLock) {
	return SQLITE_OK;
}
int gdsqlite_file::unlock(sqlite3_file *pFile, int eLock) {
	return SQLITE_OK;
}
int gdsqlite_file::checkReservedLock(sqlite3_file *pFile, int *pResOut) {
	*pResOut = 0;
	return SQLITE_OK;
}

/*
** No xFileControl() verbs are implemented by this VFS.
*/
int gdsqlite_file::fileControl(sqlite3_file *pFile, int op, void *pArg) {
	return SQLITE_NOTFOUND;
}

/*
** The xSectorSize() and xDeviceCharacteristics() methods. These two
** may return special values allowing SQLite to optimize file-system
** access to some extent. But it is also safe to simply return 0.
*/
int gdsqlite_file::sectorSize(sqlite3_file *pFile) {
	return 0;
}
int gdsqlite_file::deviceCharacteristics(sqlite3_file *pFile) {
	return 0;
}
