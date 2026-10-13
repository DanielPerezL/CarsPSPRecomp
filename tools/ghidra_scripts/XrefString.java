// Para una cadena dada, lista quién la referencia y descompila esas funciones.
// Uso headless: -postScript XrefString.java <cadena> [máx. funciones]
// @category Cars
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.util.*;

public class XrefString extends GhidraScript {
    @Override
    public void run() throws Exception {
        String target = getScriptArgs()[0];
        int max = getScriptArgs().length > 1 ? Integer.parseInt(getScriptArgs()[1]) : 2;
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        Set<Function> seen = new LinkedHashSet<>();
        for (Data d : currentProgram.getListing().getDefinedData(true)) {
            if (!d.hasStringValue() || !target.equals(StringDataInstance.getStringDataInstance(d).getStringValue())) continue;
            println("Cadena en " + d.getAddress());
            for (Reference r : getReferencesTo(d.getAddress())) {
                Function f = getFunctionContaining(r.getFromAddress());
                println("  ref desde " + r.getFromAddress() + " en " + (f == null ? "(dato)" : f.getName() + "@" + f.getEntryPoint()));
                if (f != null) seen.add(f);
            }
        }
        for (Function f : seen) {
            if (max-- <= 0) break;
            DecompileResults res = di.decompileFunction(f, 60, monitor);
            println("==== " + f.getName() + " ====\n" + res.getDecompiledFunction().getC());
        }
    }
}
