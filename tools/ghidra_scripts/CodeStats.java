// Métricas para decidir entre recompilación estática y reimplementación:
// mezcla de instrucciones (CPU/FPU/VFPU), saltos indirectos, cobertura de funciones
// y agrupación del middleware por rango de direcciones (según cadenas que referencia).
// Uso headless: -postScript CodeStats.java <salida.txt>
// @category Cars
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class CodeStats extends GhidraScript {
    static final Set<String> VFPU_MEM = new HashSet<>(Arrays.asList(
        "lv.s", "lv.q", "sv.s", "sv.q", "lvl.q", "lvr.q", "svl.q", "svr.q", "mtv", "mfv", "mtvc", "mfvc",
        "bvf", "bvt", "bvfl", "bvtl"));

    static boolean isVfpu(String m) {
        return VFPU_MEM.contains(m) || (m.startsWith("v") && !m.equals("v"));
    }

    static boolean isFpu(String m) {
        return m.endsWith(".s") && !isVfpu(m) || m.startsWith("c.") || m.equals("lwc1") || m.equals("swc1")
            || m.equals("mtc1") || m.equals("mfc1") || m.startsWith("cvt.") || m.startsWith("bc1") || m.startsWith("trunc");
    }

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(getScriptArgs()[0]);
        Listing listing = currentProgram.getListing();
        Map<String, Integer> mnem = new HashMap<>();
        long total = 0, vfpu = 0, fpu = 0, jalr = 0, jrNonRa = 0;
        Set<Function> vfpuFuncs = new HashSet<>();
        for (Instruction ins : listing.getInstructions(true)) {
            String m = ins.getMnemonicString().toLowerCase().replaceFirst("^_", "");
            total++;
            mnem.merge(m, 1, Integer::sum);
            if (isVfpu(m)) {
                vfpu++;
                Function f = getFunctionContaining(ins.getAddress());
                if (f != null) vfpuFuncs.add(f);
            } else if (isFpu(m)) fpu++;
            if (m.equals("jalr")) jalr++;
            if (m.equals("jr") && !ins.getDefaultOperandRepresentation(0).equals("ra")) jrNonRa++;
        }
        FunctionManager fm = currentProgram.getFunctionManager();
        long code = 0;
        for (MemoryBlockWrapper b : blocks()) code += b.size;
        long inFuncs = 0;
        int nf = 0;
        for (Function f : fm.getFunctions(true)) { nf++; inFuncs += f.getBody().getNumAddresses(); }
        out.printf("Instrucciones: %d  FPU: %d (%.1f%%)  VFPU: %d (%.1f%%) en %d funciones%n",
            total, fpu, 100.0 * fpu / total, vfpu, 100.0 * vfpu / total, vfpuFuncs.size());
        out.printf("Funciones: %d; bytes cubiertos por funciones: %d de %d de código ejecutable (%.1f%%)%n",
            nf, inFuncs, code, 100.0 * inFuncs / code);
        out.printf("Saltos indirectos: jalr=%d, jr no-ra=%d (tablas de salto/switch)%n", jalr, jrNonRa);
        out.println("Mnemónicos VFPU más usados:");
        mnem.entrySet().stream().filter(e -> isVfpu(e.getKey())).sorted((a, b) -> b.getValue() - a.getValue()).limit(25)
            .forEach(e -> out.printf("  %-10s %d%n", e.getKey(), e.getValue()));

        // Middleware: agrupar funciones por la familia de cadenas que referencian
        Map<String, String> families = new LinkedHashMap<>();
        families.put("Havok", "(?i).*(hk[A-Z]|havok|mopp).*");
        families.put("RenderWare", "(?i).*(\\brw[A-Z]|renderware|rwID_|rpWorld|rpSkin|rpHAnim|rtAnim).*");
        families.put("RWStudio/juego (B*)", "B[A-Z][A-Za-z0-9]{3,40}");
        families.put("2dMaestro", "(?i).*(maestro|\\bmae[A-Z]).*");
        families.put("Audio", "(?i).*(\\.vbk|\\.vbi|\\.at3|sascore|atrac).*");
        Map<String, TreeSet<Long>> famAddrs = new LinkedHashMap<>();
        for (String k : families.keySet()) famAddrs.put(k, new TreeSet<>());
        ReferenceManager rm = currentProgram.getReferenceManager();
        for (Data d : listing.getDefinedData(true)) {
            if (!d.hasStringValue()) continue;
            String s = StringDataInstance.getStringDataInstance(d).getStringValue();
            if (s == null) continue;
            for (Map.Entry<String, String> fam : families.entrySet()) {
                if (!s.matches(fam.getValue())) continue;
                for (Reference r : rm.getReferencesTo(d.getAddress())) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f != null) famAddrs.get(fam.getKey()).add(f.getEntryPoint().getOffset());
                }
            }
        }
        out.println("Funciones que referencian cadenas por familia (n, rango, percentiles 10/50/90):");
        for (Map.Entry<String, TreeSet<Long>> e : famAddrs.entrySet()) {
            List<Long> l = new ArrayList<>(e.getValue());
            if (l.isEmpty()) { out.printf("  %-22s 0%n", e.getKey()); continue; }
            out.printf("  %-22s %5d  %08x-%08x  p10=%08x p50=%08x p90=%08x%n", e.getKey(), l.size(), l.get(0), l.get(l.size() - 1),
                l.get(l.size() / 10), l.get(l.size() / 2), l.get(l.size() * 9 / 10));
        }

        // Uso directo de la GE y de syscalls
        out.println("Llamadas a imports (sitios de llamada):");
        Map<String, Integer> calls = new TreeMap<>();
        for (Function f : fm.getFunctions(true)) {
            if (!f.getName().startsWith("sce")) continue;
            int n = 0;
            for (Reference r : rm.getReferencesTo(f.getEntryPoint())) if (r.getReferenceType().isCall()) n++;
            String lib = f.getComment() == null ? "?" : f.getComment().split(" ")[0];
            calls.merge(lib, n, Integer::sum);
        }
        calls.forEach((k, v) -> out.printf("  %-24s %d%n", k, v));

        // Densidad de VFPU por rango de direcciones (bloques de 256 KB)
        out.println("VFPU por tramo de 256 KB de .text (funciones con VFPU):");
        TreeMap<Long, Integer> hist = new TreeMap<>();
        for (Function f : vfpuFuncs) hist.merge(f.getEntryPoint().getOffset() & ~0x3FFFFL, 1, Integer::sum);
        hist.forEach((k, v) -> out.printf("  %08x %d%n", k, v));
        out.close();
    }

    static class MemoryBlockWrapper { long size; MemoryBlockWrapper(long s) { size = s; } }

    List<MemoryBlockWrapper> blocks() {
        List<MemoryBlockWrapper> l = new ArrayList<>();
        for (var b : currentProgram.getMemory().getBlocks()) if (b.isExecute()) l.add(new MemoryBlockWrapper(b.getSize()));
        return l;
    }
}
