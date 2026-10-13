// Descompila las funciones en las direcciones dadas.
// Uso headless: -postScript DecompileAt.java <dir1> [dir2 ...]
// @category Cars
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (String a : getScriptArgs()) {
            Function f = getFunctionContaining(toAddr(Long.parseLong(a.replace("0x", ""), 16)));
            if (f == null) { println("Sin función en " + a); continue; }
            println("==== " + f.getName() + "@" + f.getEntryPoint() + " ====\n" + di.decompileFunction(f, 60, monitor).getDecompiledFunction().getC());
        }
    }
}
