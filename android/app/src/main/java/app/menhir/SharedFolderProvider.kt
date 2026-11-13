package app.menhir

import android.database.Cursor
import android.database.MatrixCursor
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract.Document
import android.provider.DocumentsContract.Root
import android.provider.DocumentsProvider
import android.webkit.MimeTypeMap
import java.io.File
import java.io.FileNotFoundException

/**
 * The shared folder (Storage.shared, which the Mac mounts as ~/Henge) in Android's Files app and file
 * pickers, as "Menhir shared folder". Document ids are paths relative to the folder.
 */
class SharedFolderProvider : DocumentsProvider() {
    private val rootColumns = arrayOf(
        Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS, Root.COLUMN_TITLE, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_ICON,
        Root.COLUMN_AVAILABLE_BYTES,
    )
    private val docColumns = arrayOf(
        Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_FLAGS,
        Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED,
    )

    private val base: File
        get() {
            val c = context ?: throw FileNotFoundException("no context")
            Storage.init(c)
            return Storage.shared.apply { mkdirs() }
        }

    override fun onCreate() = true

    private fun file(id: String): File {
        val f = if (id.isEmpty()) base else File(base, id)
        val canonical = f.canonicalFile
        if (!canonical.path.startsWith(base.canonicalPath)) throw FileNotFoundException(id)
        return canonical
    }

    private fun id(f: File): String = f.canonicalPath.removePrefix(base.canonicalPath).trimStart('/')

    override fun queryRoots(projection: Array<out String>?): Cursor = MatrixCursor(projection ?: rootColumns).apply {
        newRow().add(Root.COLUMN_ROOT_ID, "shared")
            .add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_CREATE or Root.FLAG_LOCAL_ONLY or Root.FLAG_SUPPORTS_IS_CHILD)
            .add(Root.COLUMN_TITLE, context?.getString(R.string.shared_folder))
            .add(Root.COLUMN_DOCUMENT_ID, "")
            .add(Root.COLUMN_ICON, R.drawable.ic_launcher)
            .add(Root.COLUMN_AVAILABLE_BYTES, base.usableSpace)
    }

    private fun row(c: MatrixCursor, f: File) {
        val dir = f.isDirectory
        var flags = Document.FLAG_SUPPORTS_DELETE or Document.FLAG_SUPPORTS_RENAME
        flags = flags or if (dir) Document.FLAG_DIR_SUPPORTS_CREATE else Document.FLAG_SUPPORTS_WRITE
        val name = if (f == base.canonicalFile) context?.getString(R.string.shared_folder) else f.name
        c.newRow().add(Document.COLUMN_DOCUMENT_ID, id(f)).add(Document.COLUMN_DISPLAY_NAME, name)
            .add(Document.COLUMN_MIME_TYPE, mime(f)).add(Document.COLUMN_FLAGS, flags)
            .add(Document.COLUMN_SIZE, if (dir) null else f.length()).add(Document.COLUMN_LAST_MODIFIED, f.lastModified())
    }

    private fun mime(f: File): String {
        if (f.isDirectory) return Document.MIME_TYPE_DIR
        val ext = f.extension.lowercase()
        return MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext) ?: "application/octet-stream"
    }

    override fun queryDocument(documentId: String, projection: Array<out String>?): Cursor =
        MatrixCursor(projection ?: docColumns).apply { row(this, file(documentId)) }

    override fun queryChildDocuments(parentDocumentId: String, projection: Array<out String>?, sortOrder: String?): Cursor =
        MatrixCursor(projection ?: docColumns).apply {
            file(parentDocumentId).listFiles()?.filter { !it.name.startsWith(".") }?.forEach { row(this, it) }
        }

    override fun openDocument(documentId: String, mode: String, signal: CancellationSignal?): ParcelFileDescriptor =
        ParcelFileDescriptor.open(file(documentId), ParcelFileDescriptor.parseMode(mode))

    override fun createDocument(parentDocumentId: String, mimeType: String, displayName: String): String {
        val parent = file(parentDocumentId)
        var f = File(parent, displayName)
        var n = 2
        while (f.exists()) f = File(parent, "${f.nameWithoutExtension.substringBeforeLast(" (")} (${n++})" + if (f.extension.isNotEmpty()) ".${f.extension}" else "")
        val ok = if (mimeType == Document.MIME_TYPE_DIR) f.mkdir() else f.createNewFile()
        if (!ok) throw FileNotFoundException("cannot create $displayName")
        return id(f)
    }

    override fun deleteDocument(documentId: String) {
        val f = file(documentId)
        if (f == base.canonicalFile || !f.deleteRecursively()) throw FileNotFoundException(documentId)
    }

    override fun renameDocument(documentId: String, displayName: String): String {
        val f = file(documentId)
        val to = File(f.parentFile, displayName)
        if (!f.renameTo(to)) throw FileNotFoundException(documentId)
        return id(to)
    }

    override fun isChildDocument(parentDocumentId: String, documentId: String): Boolean =
        file(documentId).path.startsWith(file(parentDocumentId).path)
}
