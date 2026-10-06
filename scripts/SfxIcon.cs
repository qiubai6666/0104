using System;
using System.IO;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;

// Edit only a verified module COPY, before appending the configuration/archive.
// Updating resources on an assembled SFX can discard its archive overlay.
public static class OrangeSfxIcon {
    delegate bool EnumName(IntPtr module, IntPtr type, IntPtr name, IntPtr state);
    delegate bool EnumLanguage(IntPtr module, IntPtr type, IntPtr name, ushort language, IntPtr state);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr module);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool EnumResourceNames(IntPtr module, IntPtr type, EnumName callback, IntPtr state);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool EnumResourceLanguages(IntPtr module, IntPtr type, IntPtr name, EnumLanguage callback, IntPtr state);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr BeginUpdateResource(string path, bool deleteExisting);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool UpdateResource(IntPtr update, IntPtr type, IntPtr name, ushort language, byte[] data, uint size);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool EndUpdateResource(IntPtr update, bool discard);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr FindResourceEx(IntPtr module, IntPtr type, IntPtr name, ushort language);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")] static extern IntPtr LockResource(IntPtr resource);
    [DllImport("kernel32.dll")] static extern uint SizeofResource(IntPtr module, IntPtr resource);
    sealed class Group { public int Id; public ushort Language; }
    static Exception Error() { return new Win32Exception(Marshal.GetLastWin32Error()); }
    static List<Group> Groups(IntPtr module) {
        var groups = new List<Group>();
        Exception failure = null;
        EnumName names = (m,t,n,s) => {
            if ((ulong)n.ToInt64() > 65535) { failure = new InvalidDataException("Named icon groups are unsupported."); return false; }
            EnumLanguage languages = (lm,lt,ln,l,ls) => { groups.Add(new Group {Id=n.ToInt32(), Language=l}); return true; };
            if (!EnumResourceLanguages(m,t,n,languages,IntPtr.Zero)) { failure=Error(); return false; }
            return true;
        };
        bool ok = EnumResourceNames(module,new IntPtr(14),names,IntPtr.Zero);
        if (failure != null) throw failure;
        if (!ok && Marshal.GetLastWin32Error()!=1813) throw Error();
        return groups;
    }
    static byte[] Resource(IntPtr module, int type, int id, ushort language) {
        var r=FindResourceEx(module,new IntPtr(type),new IntPtr(id),language);
        if (r==IntPtr.Zero) throw Error();
        var p=LockResource(LoadResource(module,r));
        if(p==IntPtr.Zero) throw Error();
        byte[] data=new byte[checked((int)SizeofResource(module,r))];
        Marshal.Copy(p,data,0,data.Length); return data;
    }
    static byte[][] Frames(byte[] ico, out byte[] group) {
        using(var reader=new BinaryReader(new MemoryStream(ico))) {
            if(reader.ReadUInt16()!=0 || reader.ReadUInt16()!=1) throw new InvalidDataException("Expected ICO file.");
            int count=reader.ReadUInt16();
            if(count<1 || count>256 || ico.Length<6+16*count) throw new InvalidDataException("Invalid ICO directory.");
            var frames=new byte[count][];
            using(var output=new MemoryStream()) using(var writer=new BinaryWriter(output)) {
                writer.Write((ushort)0); writer.Write((ushort)1); writer.Write((ushort)count);
                for(int i=0;i<count;i++) {
                    byte[] entry=reader.ReadBytes(8);
                    uint size=reader.ReadUInt32(), offset=reader.ReadUInt32();
                    if(size==0 || (ulong)offset+size>(ulong)ico.Length) throw new InvalidDataException("Invalid ICO frame bounds.");
                    frames[i]=new byte[checked((int)size)]; Array.Copy(ico,(long)offset,frames[i],0,(long)size);
                    writer.Write(entry); writer.Write(size); writer.Write((ushort)(30000+i));
                }
                writer.Flush(); group=output.ToArray(); return frames;
            }
        }
    }
    public static void Apply(string modulePath, string iconPath) {
        byte[] group; byte[][] frames=Frames(File.ReadAllBytes(iconPath),out group);
        IntPtr module=LoadLibraryEx(modulePath,IntPtr.Zero,2);
        if(module==IntPtr.Zero) throw Error();
        List<Group> groups;
        try { groups=Groups(module); } finally { FreeLibrary(module); }
        if(groups.Count==0) groups.Add(new Group {Id=1,Language=0});
        IntPtr update=BeginUpdateResource(modulePath,false);
        if(update==IntPtr.Zero) throw Error();
        try {
            foreach(var g in groups) {
                for(int i=0;i<frames.Length;i++)
                    if(!UpdateResource(update,new IntPtr(3),new IntPtr(30000+i),g.Language,frames[i],(uint)frames[i].Length)) throw Error();
                if(!UpdateResource(update,new IntPtr(14),new IntPtr(g.Id),g.Language,group,(uint)group.Length)) throw Error();
            }
            IntPtr commit=update; update=IntPtr.Zero;
            if(!EndUpdateResource(commit,false)) throw Error();
        } finally { if(update!=IntPtr.Zero) EndUpdateResource(update,true); }
        Verify(modulePath,iconPath);
    }
    public static void Verify(string modulePath, string iconPath) {
        byte[] expected; byte[][] frames=Frames(File.ReadAllBytes(iconPath),out expected);
        IntPtr module=LoadLibraryEx(modulePath,IntPtr.Zero,2);
        if(module==IntPtr.Zero) throw Error();
        try {
            var groups=Groups(module); if(groups.Count==0) throw new InvalidDataException("Missing icon group.");
            foreach(var g in groups) {
                Equal(expected,Resource(module,14,g.Id,g.Language));
                for(int i=0;i<frames.Length;i++) Equal(frames[i],Resource(module,3,30000+i,g.Language));
            }
        } finally { FreeLibrary(module); }
    }
    static void Equal(byte[] a, byte[] b) {
        if(a.Length!=b.Length) throw new InvalidDataException("Icon resource size mismatch.");
        for(int i=0;i<a.Length;i++) if(a[i]!=b[i]) throw new InvalidDataException("Icon resource content mismatch.");
    }
}