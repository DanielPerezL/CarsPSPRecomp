// Nombra las clases de comportamiento de RenderWare Studio (B*) a partir de su registro:
//   RWS_RegisterBehaviour(&reg, "BClase", Factoria, sizeof(BClase), 0)  (en un inicializador estático)
// La factoría hace operator_new(size, 2), llama al constructor base y fija las vtables.
// Las vtables son estilo cfront: pares {u32 ajuste de this, u32 puntero a función}.
// Uso headless: -postScript NameBehaviours.java <salida.csv>
// @category Cars
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.pcode.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class NameBehaviours extends GhidraScript {
    static final long REGISTER = 0x0880760cL, REGISTER_IMPL = 0x088073a8L, OPERATOR_NEW = 0x0880a1b4L;
    static final long ATTR_ITERATOR = 0x08806794L;
    static final int MAX_SLOTS = 96;

    DecompInterface di;
    Map<Function, HighFunction> cache = new HashMap<>();
    // función de vtable -> clases/slots que la usan
    Map<Address, List<String>> vfuncUsers = new LinkedHashMap<>();

    HighFunction high(Function f) {
        return cache.computeIfAbsent(f, k -> di.decompileFunction(k, 60, monitor).getHighFunction());
    }

    String cString(long addr) throws Exception {
        StringBuilder sb = new StringBuilder();
        Address a = toAddr(addr);
        for (int i = 0; i < 64; i++) {
            byte b = getByte(a.add(i));
            if (b == 0) break;
            sb.append((char) b);
        }
        return sb.toString();
    }

    void rename(Function f, String name) throws Exception {
        if (f != null && f.getSymbol().getSource() == SourceType.DEFAULT) f.setName(name, SourceType.ANALYSIS);
    }

    // Valor constante de un varnode del p-code de alto nivel; los punteros a globales
    // suelen llegar como PTRSUB(0, dirección), a veces envueltos en COPY/CAST.
    static Long constValue(Varnode v) {
        for (int depth = 0; v != null && depth < 4; depth++) {
            if (v.isConstant()) return v.getOffset();
            if (v.isAddress()) return v.getAddress().getOffset();
            PcodeOp def = v.getDef();
            if (def == null) return null;
            int opc = def.getOpcode();
            if (opc == PcodeOp.PTRSUB && def.getInput(0).isConstant() && def.getInput(1).isConstant())
                return def.getInput(0).getOffset() + def.getInput(1).getOffset();
            if (opc != PcodeOp.COPY && opc != PcodeOp.CAST) return null;
            v = def.getInput(0);
        }
        return null;
    }

    boolean inText(Address a) {
        MemoryBlock b = getMemoryBlock(a);
        return b != null && b.isExecute();
    }

    @Override
    public void run() throws Exception {
        di = new DecompInterface();
        di.openProgram(currentProgram);
        rename(getFunctionAt(toAddr(REGISTER)), "RWS_RegisterBehaviour");
        rename(getFunctionAt(toAddr(REGISTER_IMPL)), "RWS_RegisterBehaviourImpl");
        rename(getFunctionAt(toAddr(OPERATOR_NEW)), "operator_new");
        rename(getFunctionAt(toAddr(ATTR_ITERATOR)), "CAttributeDataChunkIterator_ctor");

        PrintWriter csv = new PrintWriter(getScriptArgs()[0]);
        csv.println("clase,tamano,static_init,factoria,vtables");
        int classes = 0;
        for (long target : new long[] {REGISTER, REGISTER_IMPL}) {
            int argBase = target == REGISTER ? 2 : 1;
            Set<Function> callers = new LinkedHashSet<>();
            for (Reference r : getReferencesTo(toAddr(target)))
                if (r.getReferenceType().isCall() && getFunctionContaining(r.getFromAddress()) != null)
                    callers.add(getFunctionContaining(r.getFromAddress()));
            for (Function caller : callers) {
                HighFunction hf = high(caller);
                if (hf == null) continue;
                Iterator<PcodeOpAST> ops = hf.getPcodeOps();
                while (ops.hasNext()) {
                    PcodeOpAST op = ops.next();
                    if (op.getOpcode() != PcodeOp.CALL || op.getInput(0).getAddress().getOffset() != target) continue;
                    if (op.getNumInputs() <= argBase + 2) continue;
                    Varnode vName = op.getInput(argBase), vFactory = op.getInput(argBase + 1), vSize = op.getInput(argBase + 2);
                    Long nameAddr = constValue(vName), factoryAddr = constValue(vFactory), size = constValue(vSize);
                    if (nameAddr == null || factoryAddr == null) continue;
                    String cls = cString(nameAddr);
                    if (!cls.matches("[A-Za-z_][A-Za-z0-9_]*")) continue;
                    Function factory = getFunctionAt(toAddr(factoryAddr));
                    if (factory == null) factory = createFunction(toAddr(factoryAddr), null);
                    rename(caller, cls + "_StaticInit");
                    rename(factory, cls + "_Create");
                    String vt = nameVtables(cls, factory);
                    nameAttributeHandlers(cls, nameAddr);
                    csv.printf("%s,0x%x,%s,%s,%s%n", cls, size == null ? -1 : size,
                        caller.getEntryPoint(), factory == null ? "" : factory.getEntryPoint(), vt);
                    classes++;
                }
            }
        }
        int own = 0, shared = 0;
        for (Map.Entry<Address, List<String>> e : vfuncUsers.entrySet()) {
            Function f = getFunctionAt(e.getKey());
            if (f == null) f = createFunction(e.getKey(), null);
            if (f == null) continue;
            List<String> users = e.getValue();
            Set<String> userClasses = new TreeSet<>();
            for (String u : users) userClasses.add(u.substring(0, u.indexOf('_')));
            if (userClasses.size() == 1) {
                rename(f, users.get(0));
                own++;
            } else {
                rename(f, "Shared_" + users.get(0).substring(users.get(0).indexOf('_') + 1));
                shared++;
            }
            String list = String.join(" ", userClasses);
            setPlateComment(f.getEntryPoint(), "Vtable de: " + (list.length() > 900 ? list.substring(0, 900) + "..." : list));
        }
        csv.close();
        println("Clases registradas: " + classes + "; métodos propios: " + own + "; compartidos/heredados: " + shared);
    }

    // Offset respecto al objeto de un puntero de STORE: (CAST|COPY)* -> PTRSUB/INT_ADD/PTRADD(base, cte)
    static long storeOffset(Varnode ptr) {
        PcodeOp def = ptr.getDef();
        while (def != null && (def.getOpcode() == PcodeOp.CAST || def.getOpcode() == PcodeOp.COPY))
            def = def.getInput(0).getDef();
        if (def == null) return 0;
        switch (def.getOpcode()) {
            case PcodeOp.PTRSUB:
            case PcodeOp.INT_ADD:
                return def.getInput(1).isConstant() ? def.getInput(1).getOffset() : -1;
            case PcodeOp.PTRADD:
                return def.getInput(1).isConstant() && def.getInput(2).isConstant()
                    ? def.getInput(1).getOffset() * def.getInput(2).getOffset() : -1;
            default:
                return 0;
        }
    }

    String nameVtables(String cls, Function factory) throws Exception {
        if (factory == null) return "";
        HighFunction hf = high(factory);
        if (hf == null) return "";
        // Por offset, la última vtable escrita es la de la clase; las anteriores son de bases (ctor inline).
        TreeMap<Long, List<Address>> byOffset = new TreeMap<>();
        List<PcodeOpAST> stores = new ArrayList<>();
        hf.getPcodeOps().forEachRemaining(op -> { if (op.getOpcode() == PcodeOp.STORE) stores.add(op); });
        stores.sort(Comparator.comparing(PcodeOpAST::getSeqnum));
        for (PcodeOpAST op : stores) {
            Long value = constValue(op.getInput(2));
            if (value == null) continue;
            Address vt = toAddr(value);
            MemoryBlock b = getMemoryBlock(vt);
            if (b == null || b.isExecute() || !b.isInitialized() || !looksLikeVtable(vt)) continue;
            long off = storeOffset(op.getInput(1));
            if (off < 0) continue;
            byOffset.computeIfAbsent(off, k -> new ArrayList<>()).add(vt);
        }
        List<String> found = new ArrayList<>();
        for (Map.Entry<Long, List<Address>> e : byOffset.entrySet()) {
            long off = e.getKey();
            List<Address> vts = e.getValue();
            Address vt = vts.get(vts.size() - 1);
            String label = String.format("%s_vtable_%x", cls, off);
            createLabel(vt, label, false, SourceType.ANALYSIS);
            found.add(label + "@" + vt + (vts.size() > 1 ? "(bases:" + (vts.size() - 1) + ")" : ""));
            for (int slot = 1; slot < MAX_SLOTS; slot++) {
                // No hay terminador: otra vtable empieza donde hay referencias entrantes.
                if (getReferencesTo(vt.add(slot * 8L)).length > 0) break;
                int delta = getInt(vt.add(slot * 8L));
                long fn = getInt(vt.add(slot * 8L + 4)) & 0xffffffffL;
                if (fn == 0 && delta == 0) continue;
                if (delta != (short) delta && delta != (delta & 0xffff)) break;
                Address fa = toAddr(fn);
                if (!inText(fa)) break;
                vfuncUsers.computeIfAbsent(fa, k -> new ArrayList<>()).add(String.format("%s_vf%x_%d", cls, off, slot));
            }
        }
        return String.join(" ", found);
    }

    boolean looksLikeVtable(Address vt) throws Exception {
        // cabecera {0,0} seguida de al menos una entrada {ajuste, función en .text}
        if (getInt(vt) != 0 || getInt(vt.add(4)) != 0) return false;
        return inText(toAddr(getInt(vt.add(12)) & 0xffffffffL));
    }

    void nameAttributeHandlers(String cls, long nameAddr) throws Exception {
        // HandleAttributes: usa CAttributeDataChunkIterator con el nombre de la clase (a través de un puntero)
        List<Address> sources = new ArrayList<>();
        sources.add(toAddr(nameAddr));
        for (Reference r : getReferencesTo(toAddr(nameAddr)))
            if (getFunctionContaining(r.getFromAddress()) == null) sources.add(r.getFromAddress());
        for (Address src : sources) {
            for (Reference r : getReferencesTo(src)) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f == null || f.getName().endsWith("_StaticInit")) continue;
                for (Function callee : f.getCalledFunctions(monitor)) {
                    if (callee.getEntryPoint().getOffset() == ATTR_ITERATOR) {
                        rename(f, cls + "_HandleAttributes");
                        break;
                    }
                }
            }
        }
    }
}
