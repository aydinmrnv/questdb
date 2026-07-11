package io.questdb.std;
import java.io.FileOutputStream;
import java.util.concurrent.atomic.AtomicInteger;
public final class KestrelDump {
    private static final String DIR = System.getProperty("KESTREL_DUMP_DIR", System.getenv("KESTREL_DUMP_DIR"));
    private static final int MAX;
    static { String m = System.getProperty("KESTREL_DUMP_MAX", System.getenv("KESTREL_DUMP_MAX")); MAX = (m == null) ? 64 : Integer.parseInt(m); }
    private static final AtomicInteger EGRESS = new AtomicInteger();
    private static final AtomicInteger INGRESS = new AtomicInteger();
    public static boolean on() { return DIR != null; }
    public static void dump(String kind, long addr, int len) {
        if (DIR == null || len <= 0) return;
        AtomicInteger seq = "egress".equals(kind) ? EGRESS : INGRESS;
        int n = seq.getAndIncrement(); if (n >= MAX) return;
        byte[] b = new byte[len];
        for (int i = 0; i < len; i++) b[i] = Unsafe.getByte(addr + i);
        try (FileOutputStream f = new FileOutputStream(String.format("%s/%s-%03d.bin", DIR, kind, n))) { f.write(b); }
        catch (Exception e) { System.err.println("KestrelDump: " + e); }
    }
}
