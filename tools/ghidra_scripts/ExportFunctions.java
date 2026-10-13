// Exporta las funciones (dirección,nombre,tamaño) a CSV.
// Uso headless: -postScript ExportFunctions.java <salida.csv>
// @category Cars
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class ExportFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        try (PrintWriter out = new PrintWriter(getScriptArgs()[0])) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true))
                out.printf("%08x,%s,%d%n", f.getEntryPoint().getOffset(), f.getName(), f.getBody().getNumAddresses());
        }
    }
}
