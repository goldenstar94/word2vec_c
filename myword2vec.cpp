// An implementation of CBOW model with negative sampling
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_STRING 100
#define EXP_TABLE_SIZE 1000
#define MAX_EXP 6
#define MAX_SENTENCE_LENGTH 1000

const int vocab_hash_size = 30000000; // Maximum 30 * 0.7 = 21M words in the vocabulary

typedef float real;

struct vocab_word{
	long long cn;    //the frequency of a word
	char *word;
}

char train_file[MAX_STRING], output_file[MAX_STRING];
char save_vocab_file[MAX_STRING], read_vocab_file[MAX_STRING];
struct vocab_word *vocab;
int binary = 0, cbow = 0, debug_mode = 2, window = 5, min_count = 5, num_threads = 1, min_reduce = 1;
int *vocab_hash;
long long vocab_max_size = 1000, vocab_size = 0, layer1_size = 100;
long long train_words = 0, word_count_actual = 0, file_size = 0;
real alpha = 0.025, starting_alpha, sample = 0;
real *syn0, *syn1neg, *expTable;
clock_t start;

int negative = 0;
const int table_size = 1e8;      //the unigram table for negative sampling
int *table;

void InitUnigramTable(){
	int a, i;
	long long train_words_pow = 0;
	real d1, power = 0.75;
	table = (int *)malloc(table_size * sizeof(int));
	if (table == NULL){
		fprintf(stderr, "cannot allocate memory for the table\n");
		exit(1);
	}
	for (a = 0; a < vocab_size; a++)
		train_words_pow += pow(vocab[a].cn,power);
	i = 0;
	d1 = pow(vocab[i].cn, power) / (real) train_words_pow;
	for (a = 0; a < table_size; a++){
		table[a] = i;
		if (a / (real) table_size > d1){
			i++;
			d1 += pow(vocab[i].cn, power) / (real) train_words_pow;
		}
		if (i >= vocab_size) i = vocab_size - 1;
	}
}

// Reads a single word from a file, assuming space + tab + EOL to be word boundaries
void ReadWord(char *word, FILE *fin) {
  int a = 0, ch;
  while (!feof(fin)) {
    ch = fgetc(fin);
    if (ch == 13) continue;
    if ((ch == ' ') || (ch == '\t') || (ch == '\n')) {
      if (a > 0) {
        if (ch == '\n') ungetc(ch, fin);
        break;
      }
      if (ch == '\n') {
        strcpy(word, (char *)"</s>");
        return;
      } else continue;
    }
    word[a] = ch;
    a++;
    if (a >= MAX_STRING - 1) a--;   // Truncate too long words
  }
  word[a] = 0;
}

// Returns hash value of a word
int GetWordHash(char *word){
	unsigned long long a, hash = 0;
	for (a = 0; a < strlen(word); a++)
		hash = hash * 257 + word[a];
	hash = hash % vocab_hash_size;
	return hash;
}

// Returns position of a word in the vocabulary; if the word is not found, return -1
int SearchVocab(char *word){
	unsigned int hash = GetWordHash(word);
	while(1){
		if (vocab_hash[hash] == -1) return -1;
		if (!strcmp(word, vocab[vocab_hash[hash]].word))
			return vocab_hash[hash];
		hash = (hash + 1) % vocab_hash_size;
	}
	return -1;
}

//Reads a word and returns its index in the vocabulary
int ReadWordIndex(FILE *fin){
	char word[MAX_STRING];
	ReadWord(word, fin);
	if (feof(fin)) return -1;
	return SearchVocab(word);
}

//Adds a word to the vocabulary
int AddWordToVocab(char *word){
	unsigned int hash, length = strlen(word) + 1;
	if (length > MAX_STRING) length = MAX_STRING;
	vocab[vocab_size].word = (char *)calloc(length, sizeof(char));
	strcpy(vocab[vocab_size].word,word);
	vocab[vocab_size].cn = 0;
	vocab_size++;
	// Reallocate memory if needed
	if (vocab_size + 2 >= vocab_max_size){
		vocab_max_size += 1000;
		vocab = (struct vocab_word *) realloc(vocab, vocab_max_size * sizeof(struct vocab_word));
	}
	hash = GetWordHash(word);
	while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
	vocab_hash[hash] = vocab_size - 1;
	return vocab_size - 1;
}

//Used later for sorting by word counts
int VocabCompare(const void *a, const void *b){
	return ((struct vocab_word *)b) ->cn - ((struct vocab_word *)a)->cn;
}

void DestroyVocab(){
	int a;
	for (a = 0; a < vocab_size; a++){
		if (vocab[a].word != NULL){
			free(vocab[a].word);
		}
	}
	free(vocab[vocab_size].word);
	free(vocab);
}

//Reduces the vocabulary by removing infrequent tokens
void ReduceVocab(){
	int a, b = 0;
	unsigned int hash;
	for (a = 0; a < vocab_size; a++) if (vocab[a].cn > min_reduce){
		vocab[b].cn = vocab[a].cn;
		vocab[b].word = vocab[a].word;
		b++;
	}else free(vocab[a].word);
	vocab_size = b;
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
	for (a = 0; a < vocab_size; a++){
		// Hash will be re-computed, as it is not actual
		hash = GetWordHash(vocab[a].word);
		while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
		vocab_hash[hash] = a;
	}
	fflush(stdout);
	min_reduce++;
}

void LearnVocabFromTrainFile(){
	char word[MAX_STRING];
	FILE *fin;
	long long a, i;
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1; //initialize word_hash array
	fin = fopen(train_file, "rb");
	if (fin == NULL){
		printf("ERROR: training data file not found !\n");
		exit(1);
	}
	vocab_size = 0;
	AddWordToVocab((char *)"</s>");
	while (1){
		ReadWord(word, fin);
		if (feof(fin)) break;
		train_words++;
		if ((debug_mode > 1) && (train_words % 100000 == 0)){
			printf("%lldK%c", train_words / 1000, 13);
			fflush(stdout);
		}
		i = SearchVocab(word);
		if (i == -1) {
			a = AddWordToVocab(word);
			vocab[a].cn = 1;
		}else vocab[i].cn++;
		if (vocab_size > vocab_hash_size * 0.7) ReduceVocab();
	}
	//SortVocab();
	if (debug_mode > 0) {
		printf("Vocab size: %lld\n", vocab_size);
		printf("Words in train file: %lld\n", train_words);
	}
	file_size = ftell(fin);
	fclose(fin);
}

void SaveVocab() {
  long long i;
  FILE *fo = fopen(save_vocab_file, "wb");
  for (i = 0; i < vocab_size; i++) fprintf(fo, "%s %lld\n", vocab[i].word, vocab[i].cn);
  fclose(fo);
}

void ReadVocab() {
  long long a, i = 0;
  char c;
  char word[MAX_STRING];
  FILE *fin = fopen(read_vocab_file, "rb");
  if (fin == NULL) {
    printf("Vocabulary file not found\n");
    exit(1);
  }
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  vocab_size = 0;
  while (1) {
    ReadWord(word, fin);
    if (feof(fin)) break;
    a = AddWordToVocab(word);
    fscanf(fin, "%lld%c", &vocab[a].cn, &c); 
    i++;
  }
  //SortVocab();
  if (debug_mode > 0) {
    printf("Vocab size: %lld\n", vocab_size);
    printf("Words in train file: %lld\n", train_words);
  }
  fin = fopen(train_file, "rb");
  if (fin == NULL) {
    printf("ERROR: training data file not found!\n");
    exit(1);
  }
  fseek(fin, 0, SEEK_END);
  file_size = ftell(fin);
  fclose(fin);
}

void InitNet(){
	long long a, b;
	a = posix_memalign((void **)&syn0, 128, (long long)vocab_size * layer1_size * sizeof(real));
	if (syn0 == NULL) {printf("Memory allocation failed\n"); exit(1);}
	a = posix_memalign((void **)&syn1neg, 128, (long long)vocab_size * layer1_size * sizeof(real));
	if (syn1neg == NULL) {printf("Memory allocation failed\n"); exit(1);}
	//Initialize the weights
	for (b = 0; b < layer1_size; b++) for (a = 0; a < vocab_size; a++)
		syn1neg[a * layer1_size + b] = 0;
	for (b = 0; b < layer1_size; b++) for (a = 0; a < vocab_size; a++)
		syn0[a * layer1_size + b] = (rand() / (real)RAND_MAX - 0.5) / layer1_size;
}

void DestroyNet(){
	if(syn0 != NULL){
		free(syn0);
	}
	if(syn1neg != NULL){
		free(syn1neg);
	}
}

void *TrainModelThread(void *id) {
	long long a, b, d, word, last_word, sentence_length = 0, sentence_position = 0;
	long long word_count = 0, last_word_count = 0, sen[MAX_SENTENCE_LENGTH + 1];
	long long l1, l2, c, target, label;
	unsigned long long next_random = (long long)id;
	real f, g;
	clock_t now;
	real *neu1 = (real *)calloc(layer1_size, sizeof(real));
	real *neu1e = (real *)calloc(layer1_size,sizeof(real));    //the gradients of the neurons
	FILE *fi = fopen(train_file,"rb");
	if (fi == NULL){
		fprintf(stderr, "no such file or directory: %s", train_file);
		exit(1);
	}
	fseek(fi,file_size / (long long) num_threads * (long long)id, SEEK_SET);
	while (1) {   
		//decay learning rate and print training progress
		if(word_count - last_word_count > 10000){
			word_count_actual += word_count - last_word_count;
			last_word_count = word_count;
			if ((debug_mode > 1)) {
				now=clock();
				printf("%cAlpha: %f  Progress: %.2f%%  Words/thread/sec: %.2fk  ", 13, alpha,
					 word_count_actual / (real)(train_words + 1) * 100,
					 word_count_actual / ((real)(now - start + 1) / (real)CLOCKS_PER_SEC * 1000));
				fflush(stdout);
			}
			alpha = starting_alpha * (1 - word_count_actual / (real)(train_words + 1));
			if (alpha < starting_alpha * 0.0001) alpha = starting_alpha * 0.0001;
		}
		// read a word sentence 
		if (sentence_length == 0){
			while (1){
				word = ReadWordIndex(fi);
				if (feof(fi)) break;
				if (word == -1) continue;
				word_count++;
				if (word == 0) break; 
				// the subsampling randomly discards frequent words while keeping the ranking same
				if (sample > 0) {
					real ran = (sqrt(vocab[word].cn / (sample * train_words)) + 1) * (sample * train_words) / vocab[word].cn;
					next_random = next_random * (unsigned long long) 25214903917 + 11;
					if (ran < (next_random & 0xFFFF) / (real)65536) continue;
				}
				sen[sentence_length] = word;
				sentence_length++;
				if (sentence_length >= MAX_SENTENCE_LENGTH) break;
			}
			sentence_position = 0;
		}
		if (feof(fi)) break;
		if (word_count > train_words / num_threads) break;
		word = sen[sentence_position];
		if (word == -1) continue;
		for (c = 0; c < layer1_size; c++) neu1[c] = 0;
		for (c = 0; c < layer1_size; c++) neu1e[c] = 0;
		next_random = next_random * (unsigned long long)25214903917 + 11;
		b = next_random % window;  //[0, window-1]
		// train the cbow model
		// in -> hidden         get contex sum vector 
		for (a = b; a < window * 2 + 1 - b; a++) if (a != window) {
			c = sentence_position - window + a;
			if (c < 0) continue;
			if (c >= sentence_length) continue;
			last_word = sen[c];
			if (last_word == -1) continue;
			for (c = 0; c < layer1_size; c++) neu1[c] += syn0[c + last_word * layer1_size];
		}
		// negative sampling 
		for (d = 0; d < negative + 1; d++){
			if (d == 0){
				target = word;
				label = 1;
			}
			else{
				next_random = next_random * (unsigned long long)25214903917 + 11;
				target = table[(next_random >> 16) % table_size];
				if (target == 0) target = next_random % (vocab_size - 1) + 1;  // if sample "</s>", randomly resample
				if (target == word) continue;
				label = 0;
			}
			l2 = target * layer1_size;
			f = 0;
			// back propagate     hidden   ->  output
			for (c = 0; c < layer1_size; c++) 
				f += neu1[c] * syn1neg[c + l2];
			if (f > MAX_EXP) 
				g = (label - 1) * alpha;
			else if (f < -MAX_EXP)
				g = (label - 0) * alpha;
			else
				g = (label - expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))]) * alpha;
			for (c = 0; c < layer1_size; c++) 
				neu1e[c] += g * syn1neg[c + l2];
			for (c = 0; c < layer1_size; c++)
				syn1neg[c + l2] += g * neu1[c];
		}
		// back propagate   hidden -> input
		for(a = b; a < window * 2 + 1 - b; a++) if (a != window){
			c = sentence_position - window + a;
			if (c < 0) continue;
			if (c >= sentence_length) continue;
			last_word = sen[c];
			if (last_word == -1) continue;
			for (c = 0; c < layer1_size; c++)
				syn0[c + last_word * layer1_size] += neu1e[c];
		}
		sentence_position++;
		if (sentence_position >= sentence_length){
			sentence_length = 0;
			continue;
		}
	}
	fclose(fi);
	free(neu1);
	free(neu1e);
	pthread_exit(NULL);
}

void TrainModel(){
	long a, b, c, d;
	FILE *fo;
	pthread_t *pt = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
	if (pt == NULL){
		fprintf(stderr, "cannot allocate memory for threads\n");
		exit(1);
	}
	printf("Starting training using file %s \n", train_file);
	starting_alpha = alpha;
	if (read_vocab_file[0] != 0) ReadVocab();
	else LearnVocabFromTrainFile();
	if (save_vocab_file[0] != 0) SaveVocab();
	if (output_file[0] == 0) return;
	InitNet();
	if (negative > 0) InitUnigramTable();
	start = clock();
	for (a = 0; a < num_threads; a++) pthread_create(&pt[a], NULL, TrainModelThread, (void *)a);
	for (a = 0; a < num_threads; a++) pthread_join(pt[a], NULL);
	fo = fopen(output_file, "wb");
	if (fo == NULL) {
		fprintf(stderr, "Cannot open %s: permission denied\n", output_file);
		exit(1);
	}
	// save the word vectors
	fprintf(fo, "%lld %lld\n", vocab_size, layer1_size);
	for (a = 0; a < vocab_size; a++) {
		if (vocab[a].word != NULL)
			fprintf(fo, "%s ", vocab[a].word);
		if (binary)
			for (b = 0; b < layer1_size; b++) fwrite(&syn0[a * layer1_size + b], sizeof(real), 1, fo);
		else
			for (b = 0; b < layer1_size; b++) fprintf(fo, "%lf ", syn0[a * layer1_size + b]);
		fprintf(fo, "\n");
	}
	fclose(fo);
	free(table);
	free(pt);
	DestroyVocab();
}

int ArgPos(char *str, int argc, char **argv) {
  int a;
  for (a = 1; a < argc; a++) if (!strcmp(str, argv[a])) {
    if (a == argc - 1) {
      printf("Argument missing for %s\n", str);
      exit(1);
    }
    return a;
  }
  return -1;
}

int main(int argc, char **argv) {
	int i;
	if(argc == 1){
		printf("WORD VECTOR estimation toolkit v 0.1b\n\n");
    	printf("Options:\n");
    	printf("Parameters for training:\n");
    	printf("\t-train <file>\n");
    	printf("\t\tUse text data from <file> to train the model\n");
    	printf("\t-output <file>\n");
    	printf("\t\tUse <file> to save the resulting word vectors / word clusters\n");
    	printf("\t-size <int>\n");
    	printf("\t\tSet size of word vectors; default is 100\n");
    	printf("\t-window <int>\n");
    	printf("\t\tSet max skip length between words; default is 5\n");
    	printf("\t-sample <float>\n");
    	printf("\t\tSet threshold for occurrence of words. Those that appear with higher frequency");
    	printf(" in the training data will be randomly down-sampled; default is 0 (off), useful value is 1e-5\n");
    	printf("\t-negative <int>\n");
    	printf("\t\tNumber of negative examples; default is 0, common values are 5 - 10 (0 = not used)\n");
    	printf("\t-threads <int>\n");
    	printf("\t\tUse <int> threads (default 1)\n");
    	printf("\t-min-count <int>\n");
    	printf("\t\tThis will discard words that appear less than <int> times; default is 5\n");
    	printf("\t-alpha <float>\n");
    	printf("\t\tSet the starting learning rate; default is 0.025\n");
    	printf("\t-debug <int>\n");
    	printf("\t\tSet the debug mode (default = 2 = more info during training)\n");
    	printf("\t-binary <int>\n");
    	printf("\t\tSave the resulting vectors in binary moded; default is 0 (off)\n");
    	printf("\t-save-vocab <file>\n");
    	printf("\t\tThe vocabulary will be saved to <file>\n");
    	printf("\t-read-vocab <file>\n");
    	printf("\t\tThe vocabulary will be read from <file>, not constructed from the training data\n");
    	printf("\nExamples:\n");
    	printf("./word2vec -train data.txt -output vec.txt -debug 2 -size 200 -window 5 -sample 1e-4 -negative 5 -hs 0 -binary 0 -cbow 1\n\n");
    	return 0;
	}
	output_file[0] = 0;
  	save_vocab_file[0] = 0;
  	read_vocab_file[0] = 0;
  	if ((i = ArgPos((char *)"-size", argc, argv)) > 0) layer1_size = atoi(argv[i + 1]);
  	if ((i = ArgPos((char *)"-train", argc, argv)) > 0) strcpy(train_file, argv[i + 1]);
  	if ((i = ArgPos((char *)"-save-vocab", argc, argv)) > 0) strcpy(save_vocab_file, argv[i + 1]);
  	if ((i = ArgPos((char *)"-read-vocab", argc, argv)) > 0) strcpy(read_vocab_file, argv[i + 1]);
  	if ((i = ArgPos((char *)"-debug", argc, argv)) > 0) debug_mode = atoi(argv[i + 1]);
  	if ((i = ArgPos((char *)"-binary", argc, argv)) > 0) binary = atoi(argv[i + 1]);
  	if ((i = ArgPos((char *)"-alpha", argc, argv)) > 0) alpha = atof(argv[i + 1]);
  	if ((i = ArgPos((char *)"-output", argc, argv)) > 0) strcpy(output_file, argv[i + 1]);
  	if ((i = ArgPos((char *)"-window", argc, argv)) > 0) window = atoi(argv[i + 1]);
  	if ((i = ArgPos((char *)"-sample", argc, argv)) > 0) sample = atof(argv[i + 1]);
  	if ((i = ArgPos((char *)"-negative", argc, argv)) > 0) negative = atoi(argv[i + 1]);
  	if ((i = ArgPos((char *)"-threads", argc, argv)) > 0) num_threads = atoi(argv[i + 1]);
  	if ((i = ArgPos((char *)"-min-count", argc, argv)) > 0) min_count = atoi(argv[i + 1]);
  	vocab = (struct vocab_word *)calloc(vocab_max_size, sizeof(struct vocab_word));
  	vocab_hash = (int *)calloc(vocab_hash_size, sizeof(int));
  	expTable = (real *)malloc((EXP_TABLE_SIZE + 1) * sizeof(real));
  	if (expTable == NULL) {
    	fprintf(stderr, "out of memory\n");
    	exit(1);
  	}
  	for (i = 0; i < EXP_TABLE_SIZE; i++) {
    	expTable[i] = exp((i / (real)EXP_TABLE_SIZE * 2 - 1) * MAX_EXP); // Precompute the exp() table
    	expTable[i] = expTable[i] / (expTable[i] + 1);                   // Precompute f(x) = x / (x + 1)
  	}
  	TrainModel();
  	DestroyNet();
  	free(vocab_hash);
  	free(expTable);
  	return 0;
}
