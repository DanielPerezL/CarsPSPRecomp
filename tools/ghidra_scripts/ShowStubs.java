// Muestra bloques de memoria y los símbolos de los stubs de import (.sceStub.text.*).
// @category Cars
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;

public class ShowStubs extends GhidraScript {
    @Override
    public void run() throws Exception {
        println("Base: " + currentProgram.getImageBase());
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (!b.getName().startsWith(".sceStub")) continue;
            StringBuilder sb = new StringBuilder(b.getName() + " " + b.getStart() + ":");
            SymbolIterator it = currentProgram.getSymbolTable().getSymbols(new ghidra.program.model.address.AddressSet(b.getStart(), b.getEnd()), SymbolType.LABEL, true);
            int n = 0;
            while (it.hasNext() && n < 4) { Symbol s = it.next(); sb.append(" ").append(s.getName(true)); n++; }
            SymbolIterator it2 = currentProgram.getSymbolTable().getSymbols(new ghidra.program.model.address.AddressSet(b.getStart(), b.getEnd()), SymbolType.FUNCTION, true);
            while (it2.hasNext() && n < 8) { Symbol s = it2.next(); sb.append(" F:").append(s.getName(true)); n++; }
            println(sb.toString());
        }
    }
}
