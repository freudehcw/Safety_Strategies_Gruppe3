DOCNAME = Campus26_Gruppe3_Designdokument
MAIN = start.tex

all: $(DOCNAME).pdf

$(DOCNAME).pdf: $(MAIN) *.tex
	pdflatex -jobname=$(DOCNAME) $(MAIN)
	pdflatex -jobname=$(DOCNAME) $(MAIN)

clean:
	rm -f *.aux *.log *.out *.toc *.lof *.lot *.fls *.fdb_latexmk $(DOCNAME).pdf
