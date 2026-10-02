DOCNAME = Campus26_Gruppe3_Designdokument
MAIN = start

all: $(DOCNAME).pdf

$(DOCNAME).pdf: $(MAIN) *.tex
	pdflatex -jobname=$(DOCNAME) $(MAIN).tex
	pdflatex -jobname=$(DOCNAME) $(MAIN).tex

clean:
	rm -f *.aux *.log *.out *.toc *.lof *.lot *.fls *.fdb_latexmk $(MAIN).pdf
