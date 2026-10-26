extern int decode_entry(const unsigned char* p, unsigned long n);
int main(void) { static const unsigned char b[] = {0x81, 0x00}; return decode_entry(b, 2); }
