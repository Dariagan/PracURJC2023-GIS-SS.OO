
typedef struct
{
	char * filename;
	int argc; //nº args del comando
	char ** argv; //array de args
} tcommand;

typedef struct
{
	int ncommands; //nº comandos a ejecutar
	tcommand * commands;//lista de comandos a ejecutar contenidos, pertenecientes a la línea
	char * redirect_input; //usado cuando al comando se le hace comer un fichero como input
	char * redirect_output;//nombre fichero en donde se vomita stdout
	char * redirect_error;//nombre fichero en donde se vomita stderr
	int background; //si se manda toda la linea al background (1) o no (0)
} tline;

extern tline* tokenize(char *str);

