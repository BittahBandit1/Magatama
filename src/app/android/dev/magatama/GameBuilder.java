// Builds the game on the phone from the user's own disc, as the macOS installer does on a Mac
// (src/launcher/macos-installer/magatama_install.py): the APK carries no game code. It
//   1. unpacks the build kit (prebuilt runtime, headers, recipe: tools/android/magatama_android_kit.py) and the
//      compiler's files (sysroot, clang's resource directory: tools/android/stage_toolchain.py) from the APK's assets,
//   2. translates the game code from Documents/Magatama/disc/SLUS_209.11 with the recompiler and the generators
//      (executables in the APK's lib/ folder: libmg_recomp.so, libmg_vuaot.so, libmg_effects.so),
//   3. compiles it with clang 21 (libmg_clang.so) on every core it can afford,
//   4. links the game, files/game/libmain.so, with lld (libmg_lld.so); MagatamaActivity loads it from there.
// The translated code stays in the app's private storage. The compiled objects are kept, so an app update that
// does not change how the game code is compiled only links again.
package dev.magatama;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.os.Build;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class GameBuilder {
    static final String[] STEPS = {"Unpacking the compiler", "Translating the game code", "Compiling", "Linking"};

    interface Listener {
        void step(int index, String name);
        void progress(int done, int total, String what);
        void log(String line);
    }

    static final class BuildException extends Exception {
        BuildException(String message) {
            super(message);
        }
    }

    private final Context context;
    private final File disc;
    private final Listener listener;
    private final AtomicBoolean cancelled = new AtomicBoolean();
    private final List<Process> running = new ArrayList<>();
    private FileOutputStream logFile;

    GameBuilder(Context context, File disc, Listener listener) {
        this.context = context.getApplicationContext();
        this.disc = disc;
        this.listener = listener;
    }

    // ---------------------------------------------------------------------------------------------- where things are
    static File gameDir(Context c) {
        return new File(c.getFilesDir(), "game");
    }

    static File gameLibrary(Context c) {
        return new File(gameDir(c), "libmain.so");
    }

    private static File stampFile(Context c) {
        return new File(gameDir(c), "build.stamp");
    }

    static long appVersion(Context c) {
        try {
            PackageInfo info = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            return Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : info.versionCode;
        } catch (Exception e) {
            return 0;
        }
    }

    /** True when files/game/libmain.so was built by this version of the app (an update needs a new link). */
    static boolean built(Context c) {
        try {
            return gameLibrary(c).isFile() &&
                new String(Files.readAllBytes(stampFile(c).toPath()), StandardCharsets.UTF_8).trim()
                    .equals("app " + appVersion(c));
        } catch (IOException e) {
            return false;
        }
    }

    static File logPath(Context c) {
        return new File(c.getFilesDir(), "build/build.log");
    }

    void cancel() {
        cancelled.set(true);
        synchronized (running) {
            for (Process p : running)
                p.destroy();
        }
    }

    // ---------------------------------------------------------------------------------------------- the build
    void run() throws BuildException {
        final File root = new File(context.getFilesDir(), "build");
        root.mkdirs();
        try {
            logFile = new FileOutputStream(logPath(context));
        } catch (IOException e) {
            throw new BuildException("Cannot write in the app's storage: " + e.getMessage());
        }
        final long started = System.currentTimeMillis();
        try {
            build(root);
            log("built in " + (System.currentTimeMillis() - started) / 1000 + " s");
        } finally {
            try {
                logFile.close();
            } catch (IOException ignored) {
            }
        }
    }

    private void build(File root) throws BuildException {
        final File elf = new File(disc, "SLUS_209.11");
        if (!elf.isFile())
            throw new BuildException("Import your disc first.");
        final String nativeDir = context.getApplicationInfo().nativeLibraryDir;
        final File bin = new File(root, "bin");

        // 1. the kit and the compiler's files (again whenever the app changed)
        listener.step(0, STEPS[0]);
        final String version = "app " + appVersion(context);
        final File kit = new File(root, "kit"), toolchain = new File(root, "toolchain");
        final File unpacked = new File(root, "unpacked.stamp");
        if (!version.equals(readText(unpacked))) {
            deleteTree(kit);
            deleteTree(toolchain);
            unzipAsset("build/kit.zip", kit, 0, 2);
            unzipAsset("build/toolchain.zip", toolchain, 1, 2);
            writeText(unpacked, version);
        }
        // Tools that pick their behaviour from their own name run through symlinks (lld: ld.lld; clang: clang++).
        bin.mkdirs();
        symlink(new File(nativeDir, "libmg_clang.so"), new File(bin, "clang++"));
        symlink(new File(nativeDir, "libmg_lld.so"), new File(bin, "ld.lld"));
        final JSONObject recipe;
        try {
            recipe = new JSONObject(readText(new File(kit, "recipe.json")));
        } catch (Exception e) {
            throw new BuildException("The build kit is damaged: " + e.getMessage());
        }

        // The compiled game objects are reused when the disc and the way they are compiled are the same.
        final String objectsKey = sha256(fileDigest(elf) + "\n" + recipe.optJSONObject("unity") +
                                         recipe.optJSONObject("ps2_vu1") + recipe.optJSONObject("rrv_stubs") +
                                         recipe.optJSONObject("nocturne_bezier"));
        final File work = new File(root, "work"), gen = new File(work, "generated"), obj = new File(work, "obj");
        final File objStamp = new File(work, "objects.stamp");
        final boolean reuse = objectsKey.equals(readText(objStamp));

        final Fill fill = new Fill(kit, gen, obj, new File(toolchain, "sysroot"), new File(bin, "clang++"));
        if (!reuse) {
            deleteTree(work);
            // 2. the game code: the recompiler, the VU microcode, the routines the function set leaves out
            listener.step(1, STEPS[1]);
            final File output = new File(gen, "output");
            for (String sub : new String[] {"output", "vu", "legacy-stubs", "native", "effects"})
                new File(gen, sub).mkdirs();
            String config = readText(new File(kit, "config/nocturne-recomp.toml.in"));
            config = config.replace("@ELF@", elf.getAbsolutePath()).replace("@OUTPUT@", output.getAbsolutePath())
                         .replace("@FUNCTIONS@", new File(kit, "config/nocturne-functions.csv").getAbsolutePath());
            final File toml = new File(work, "nocturne.toml");
            writeText(toml, config);
            listener.progress(0, 3, "the game's functions");
            exec(Arrays.asList(new File(nativeDir, "libmg_recomp.so").getPath(), toml.getPath()), work, "ps2_recomp");
            copy(new File(output, "ps2_recompiled_stubs.h"), new File(gen, "legacy-stubs/ps2_recompiled_stubs.h"));
            listener.progress(1, 3, "the VU microcode");
            exec(Arrays.asList(new File(nativeDir, "libmg_vuaot.so").getPath(), "--elf", elf.getPath(), "--output",
                               new File(gen, "vu").getPath()), work, "vu_aot_gen");
            listener.progress(2, 3, "the battle and effect routines");
            exec(Arrays.asList(new File(nativeDir, "libmg_effects.so").getPath(), elf.getPath(),
                               new File(gen, "effects/nocturne_bezier.cpp").getPath()), work, "nocturne-effects-generate");
            listener.progress(3, 3, "");

            // 3. compile: the game functions in batches of 50, plus the three files that include generated code
            listener.step(2, STEPS[2]);
            obj.mkdirs();
            final File unity = new File(work, "unity");
            unity.mkdirs();
            final String[] names = output.list((d, n) -> n.endsWith(".cpp"));
            if (names == null || names.length == 0)
                throw new BuildException("The recompiler produced no code (see the build log).");
            Arrays.sort(names);
            final List<String[]> jobs = new ArrayList<>(); // kind, source (or null), object
            for (int i = 0; i < names.length; i += 50) {
                final StringBuilder text = new StringBuilder();
                for (int j = i; j < Math.min(i + 50, names.length); j++)
                    text.append("#include \"").append(new File(output, names[j]).getPath()).append("\"\n");
                final File u = new File(unity, "unity_" + (i / 50) + "_cxx.cxx");
                writeText(u, text.toString());
                jobs.add(new String[] {"unity", u.getPath(), new File(obj, "unity_" + (i / 50) + ".o").getPath()});
            }
            jobs.add(new String[] {"nocturne_bezier", new File(gen, "effects/nocturne_bezier.cpp").getPath(),
                                   new File(obj, "nocturne_bezier.o").getPath()});
            jobs.add(new String[] {"ps2_vu1", null, new File(obj, "ps2_vu1.o").getPath()});
            jobs.add(new String[] {"rrv_stubs", null, new File(obj, "rrv_stubs.o").getPath()});
            compileAll(recipe, fill, jobs, new File(toolchain, "clang"), work);
            writeText(objStamp, objectsKey);
        }

        // 4. link
        listener.step(3, STEPS[3]);
        listener.progress(0, 1, "libmain.so");
        final File staging = new File(work, "libmain.so");
        final List<String> cmd = new ArrayList<>();
        try {
            final JSONArray link = recipe.getJSONArray("link");
            final File[] unityObjects = obj.listFiles((d, n) -> n.startsWith("unity_") && n.endsWith(".o"));
            if (unityObjects == null)
                throw new BuildException("The compiled game code is missing.");
            Arrays.sort(unityObjects);
            for (int i = 0; i < link.length(); i++) {
                final String token = link.getString(i);
                if (token.equals("@OBJ@/libmagatama_game.a")) { // the game objects themselves (no archiver here)
                    for (File o : unityObjects)
                        cmd.add(o.getPath());
                    continue;
                }
                cmd.add(fill.apply(token).replace("@OUT@", staging.getPath()));
                if (i == 0) {
                    cmd.add("-resource-dir");
                    cmd.add(new File(toolchain, "clang").getPath());
                    cmd.add("--ld-path=" + new File(bin, "ld.lld").getPath());
                }
            }
        } catch (BuildException e) {
            throw e;
        } catch (Exception e) {
            throw new BuildException("The build kit's link command is damaged: " + e.getMessage());
        }
        exec(cmd, work, "the link");
        // Termux's clang driver adds -rpath=<sysroot>/usr/lib (Termux's own prefix): that folder holds the NDK's link
        // stubs, which the dynamic linker must never load in place of the system's libraries. Point it at $ORIGIN.
        replaceInFile(staging, (new File(toolchain, "sysroot").getPath() + "/usr/lib").getBytes(StandardCharsets.UTF_8),
                      "$ORIGIN".getBytes(StandardCharsets.UTF_8));
        final File game = gameDir(context);
        game.mkdirs();
        final File library = gameLibrary(context);
        if (!staging.renameTo(library)) {
            copy(staging, library);
            staging.delete();
        }
        writeText(stampFile(context), version);
        listener.progress(1, 1, "libmain.so");
    }

    private void compileAll(JSONObject recipe, Fill fill, List<String[]> jobs, File resourceDir, File work)
        throws BuildException {
        // As many compilers as there are cores, and as memory allows (a batch of 50 functions takes up to ~1 GB).
        int threads = Runtime.getRuntime().availableProcessors();
        try {
            android.app.ActivityManager am =
                (android.app.ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
            android.app.ActivityManager.MemoryInfo mem = new android.app.ActivityManager.MemoryInfo();
            am.getMemoryInfo(mem);
            threads = (int) Math.max(1, Math.min(threads, mem.availMem / (1100L << 20)));
        } catch (Exception ignored) {
        }
        log("compiling " + jobs.size() + " units on " + threads + " threads");
        final ExecutorService pool = Executors.newFixedThreadPool(threads);
        final AtomicInteger done = new AtomicInteger();
        final List<Future<String>> results = new ArrayList<>();
        for (final String[] job : jobs) {
            results.add(pool.submit(() -> {
                if (cancelled.get())
                    return "cancelled";
                final JSONObject t = recipe.getJSONObject(job[0]);
                final String source = job[1] != null ? job[1] : fill.apply(t.getString("source"));
                final JSONArray command = t.getJSONArray("command");
                final List<String> cmd = new ArrayList<>();
                for (int i = 0; i < command.length(); i++) {
                    cmd.add(fill.apply(command.getString(i)).replace("@INPUT@", source).replace("@OUTPUT@", job[2]));
                    if (i == 0) {
                        cmd.add("-resource-dir");
                        cmd.add(resourceDir.getPath());
                    }
                }
                try {
                    exec(cmd, work, "compiling " + new File(job[1] != null ? job[1] : source).getName());
                } catch (BuildException e) {
                    return e.getMessage();
                }
                listener.progress(done.incrementAndGet(), jobs.size(), new File(job[2]).getName());
                return null;
            }));
        }
        pool.shutdown();
        String failure = null;
        for (Future<String> f : results) {
            try {
                final String r = f.get();
                if (r != null && failure == null) {
                    failure = r;
                    cancel(); // stop the others
                }
            } catch (Exception e) {
                if (failure == null)
                    failure = e.toString();
            }
        }
        if (failure != null)
            throw new BuildException(failure);
    }

    /** The recipe's placeholders, for this phone. */
    private static final class Fill {
        final String kit, gen, obj, sysroot, cxx;

        Fill(File kit, File gen, File obj, File sysroot, File cxx) {
            this.kit = kit.getPath();
            this.gen = gen.getPath();
            this.obj = obj.getPath();
            this.sysroot = sysroot.getPath();
            this.cxx = cxx.getPath();
        }

        String apply(String token) {
            return token.replace("@KIT@", kit).replace("@GEN@", gen).replace("@OBJ@", obj)
                .replace("@SYSROOT@", sysroot).replace("@CXX@", cxx);
        }
    }

    // ---------------------------------------------------------------------------------------------- helpers
    private void exec(List<String> cmd, File dir, String what) throws BuildException {
        if (cancelled.get())
            throw new BuildException("cancelled");
        final ProcessBuilder pb = new ProcessBuilder(cmd).directory(dir).redirectErrorStream(true);
        pb.environment().put("TMPDIR", context.getCacheDir().getPath());
        final Process p;
        try {
            p = pb.start();
        } catch (IOException e) {
            throw new BuildException(what + " could not start: " + e.getMessage());
        }
        synchronized (running) {
            running.add(p);
        }
        final StringBuilder tail = new StringBuilder();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                log(line);
                tail.append(line).append('\n');
                if (tail.length() > 4000)
                    tail.delete(0, tail.length() - 3000);
            }
            final int code = p.waitFor();
            if (cancelled.get())
                throw new BuildException("cancelled");
            if (code != 0)
                throw new BuildException(what + " failed (" + code + "):\n" + lastLines(tail.toString(), 12));
        } catch (IOException | InterruptedException e) {
            throw new BuildException(what + " was interrupted: " + e.getMessage());
        } finally {
            synchronized (running) {
                running.remove(p);
            }
        }
    }

    private synchronized void log(String line) {
        try {
            logFile.write((line + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
        listener.log(line);
    }

    private static String lastLines(String text, int n) {
        final String[] lines = text.trim().split("\n");
        return String.join("\n", Arrays.copyOfRange(lines, Math.max(0, lines.length - n), lines.length));
    }

    private void unzipAsset(String asset, File dest, int part, int parts) throws BuildException {
        final File tmp = new File(dest.getPath() + ".partial");
        deleteTree(tmp);
        tmp.mkdirs();
        long total = 0;
        try (InputStream in = context.getAssets().open(asset)) {
            total = Math.max(1, in.available());
        } catch (IOException ignored) {
        }
        try (ZipInputStream zip = new ZipInputStream(context.getAssets().open(asset))) {
            final byte[] buffer = new byte[1 << 16];
            ZipEntry e;
            long read = 0;
            final String base = tmp.getCanonicalPath() + File.separator;
            while ((e = zip.getNextEntry()) != null) {
                final File f = new File(tmp, e.getName());
                if (!f.getCanonicalPath().startsWith(base))
                    throw new IOException("bad entry " + e.getName());
                if (e.isDirectory()) {
                    f.mkdirs();
                    continue;
                }
                f.getParentFile().mkdirs();
                try (OutputStream out = new FileOutputStream(f)) {
                    int n;
                    while ((n = zip.read(buffer)) > 0) {
                        out.write(buffer, 0, n);
                    }
                }
                read += Math.max(0, e.getCompressedSize());
                listener.progress((int) (part * 1000 + Math.min(999, read * 1000 / total)), parts * 1000, e.getName());
                if (cancelled.get())
                    throw new BuildException("cancelled");
            }
        } catch (IOException e) {
            throw new BuildException("Could not unpack " + asset + ": " + e.getMessage());
        }
        if (!tmp.renameTo(dest))
            throw new BuildException("Could not unpack " + asset);
    }

    /** Overwrites every NUL-terminated occurrence of `from` with `to` (shorter) and NUL padding. The file is mapped,
     *  not read into the heap: libmain.so is ~160 MB, more than an app's Java heap allows (b41 on the Nova: OOM). */
    private static void replaceInFile(File f, byte[] from, byte[] to) throws BuildException {
        try (java.nio.channels.FileChannel ch = java.nio.channels.FileChannel.open(f.toPath(),
                 java.nio.file.StandardOpenOption.READ, java.nio.file.StandardOpenOption.WRITE)) {
            final long size = ch.size();
            if (size > Integer.MAX_VALUE)
                throw new BuildException("Cannot finish " + f.getName() + ": too large");
            final java.nio.MappedByteBuffer data = ch.map(java.nio.channels.FileChannel.MapMode.READ_WRITE, 0, size);
            final int n = (int) size;
            final byte first = from[0];
            boolean changed = false;
            outer:
            for (int i = 0; i + from.length < n; i++) {
                if (data.get(i) != first)
                    continue;
                for (int j = 1; j < from.length; j++)
                    if (data.get(i + j) != from[j])
                        continue outer;
                if (data.get(i + from.length) != 0 || (i > 0 && data.get(i - 1) != 0))
                    continue;
                for (int j = 0; j < from.length; j++)
                    data.put(i + j, j < to.length ? to[j] : 0);
                changed = true;
            }
            if (changed)
                data.force();
        } catch (IOException e) {
            throw new BuildException("Cannot finish " + f.getName() + ": " + e.getMessage());
        }
    }

    private static void symlink(File target, File link) throws BuildException {
        try {
            link.delete();
            android.system.Os.symlink(target.getPath(), link.getPath());
        } catch (Exception e) {
            throw new BuildException("Could not prepare the compiler: " + e.getMessage());
        }
    }

    static void deleteTree(File f) {
        if (f == null || !f.exists())
            return;
        if (f.isDirectory() && !Files.isSymbolicLink(f.toPath())) {
            final File[] children = f.listFiles();
            if (children != null)
                for (File c : children)
                    deleteTree(c);
        }
        f.delete();
    }

    private static String readText(File f) {
        try {
            return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeText(File f, String text) throws BuildException {
        try {
            f.getParentFile().mkdirs();
            Files.write(f.toPath(), text.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            throw new BuildException("Cannot write " + f + ": " + e.getMessage());
        }
    }

    private static void copy(File from, File to) throws BuildException {
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
            final byte[] buffer = new byte[1 << 16];
            int n;
            while ((n = in.read(buffer)) > 0)
                out.write(buffer, 0, n);
        } catch (IOException e) {
            throw new BuildException("Cannot copy " + from.getName() + ": " + e.getMessage());
        }
    }

    private static String fileDigest(File f) throws BuildException {
        try (InputStream in = new FileInputStream(f)) {
            final MessageDigest md = MessageDigest.getInstance("SHA-256");
            final byte[] buffer = new byte[1 << 16];
            int n;
            while ((n = in.read(buffer)) > 0)
                md.update(buffer, 0, n);
            return hex(md.digest());
        } catch (Exception e) {
            throw new BuildException("Cannot read " + f + ": " + e.getMessage());
        }
    }

    private static String sha256(String text) {
        try {
            return hex(MessageDigest.getInstance("SHA-256").digest(text.getBytes(StandardCharsets.UTF_8)));
        } catch (Exception e) {
            return text;
        }
    }

    private static String hex(byte[] bytes) {
        final StringBuilder s = new StringBuilder();
        for (byte b : bytes)
            s.append(String.format("%02x", b));
        return s.toString();
    }
}
