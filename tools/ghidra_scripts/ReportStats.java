// Resumen del análisis: funciones, imports resueltos y referencias a nombres de clases B*.
// @category Cars
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.StringDataInstance;
import java.util.*;

public class ReportStats extends GhidraScript {
    @Override
    public void run() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        int total = 0, named = 0;
        Map<String, Integer> prefixes = new TreeMap<>();
        for (Function f : fm.getFunctions(true)) {
            total++;
            if (!f.getName().startsWith("FUN_")) {
                named++;
                String p = f.getName().contains("_") ? f.getName().substring(0, f.getName().indexOf('_')) : f.getName();
                prefixes.merge(p, 1, Integer::sum);
            }
        }
        println("Funciones: " + total + " (con nombre: " + named + ")");
        prefixes.entrySet().stream().sorted((a, b) -> b.getValue() - a.getValue()).limit(15)
            .forEach(e -> println("  " + e.getKey() + ": " + e.getValue()));

        int strings = 0, classRefs = 0, classesWithRefs = 0;
        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        for (Data d : listing.getDefinedData(true)) {
            if (!d.hasStringValue()) continue;
            strings++;
            String s = StringDataInstance.getStringDataInstance(d).getStringValue();
            if (s != null && s.matches("B[A-Z][A-Za-z0-9]{3,40}")) {
                int n = rm.getReferenceCountTo(d.getAddress());
                classRefs += n;
                if (n > 0) classesWithRefs++;
            }
        }
        println("Cadenas definidas: " + strings + "; clases B* referenciadas desde código: " + classesWithRefs + " (" + classRefs + " refs)");
    }
}
