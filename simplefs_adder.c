#include "simplefs.h"

void set_bit(unsigned char *bitmap, int index) { bitmap[index / 8] |= (1u << (index % 8)); }
int is_bit_set(unsigned char *bitmap, int index) { return bitmap[index / 8] & (1u << (index % 8)); }
long inode_offset(int inode_number) { return ((long)INODE_TABLE_BLOCK * BLOCK_SIZE) + ((long)(inode_number - 1) * sizeof(inode_t)); }
int data_bitmap_index(int absolute_block_number) { return absolute_block_number - DATA_REGION_BLOCK; }

int find_free_inode(unsigned char *bitmap)
{
    
    for (int i = 1; i < TOTAL_INODES; i++) {
        if (!is_bit_set(bitmap, i)) {
            return i + 1; 
        }
    }
    
    return -1;
}

int find_free_data_block(unsigned char *bitmap)
{
   
    for (int i = 0; i < DATA_BLOCKS; i++) {
        if (!is_bit_set(bitmap, i)) {
            return DATA_REGION_BLOCK + i;
        }
    }
    
    return -1;
}

int filename_exists(FILE *image, const char *filename)
{
    dirent_t entry;
    
    for (int i = 0; i < (int)(BLOCK_SIZE / sizeof(dirent_t)); i++) {
        long pos = ((long)ROOT_DATA_BLOCK * BLOCK_SIZE) + ((long)i * sizeof(dirent_t));
        fseek(image, pos, SEEK_SET);
        if (fread(&entry, sizeof(entry), 1, image) != 1) break;
        if (entry.inode_no != 0 && strcmp(entry.name, filename) == 0) {
            return 1;
        }
    }
    
    return 0;
}

int find_free_directory_entry(FILE *image)
{
    dirent_t entry;
    
    for (int i = 2; i < (int)(BLOCK_SIZE / sizeof(dirent_t)); i++) {
        long pos = ((long)ROOT_DATA_BLOCK * BLOCK_SIZE) + ((long)i * sizeof(dirent_t));
        fseek(image, pos, SEEK_SET);
        if (fread(&entry, sizeof(entry), 1, image) != 1) break;
        if (entry.inode_no == 0) {
            return i;
        }
    }
    
    return -1;
}

int main(int argc, char *argv[])
{
    char *image_name = NULL, *source_name = NULL;
    FILE *image, *source;
    superblock_t sb;
    unsigned char inode_bitmap[BLOCK_SIZE], data_bitmap[BLOCK_SIZE];
    inode_t new_inode, root_inode;
    dirent_t new_entry;
    long file_size;
    int required_blocks, free_inode;
    int allocated_blocks[MAX_DIRECT_BLOCKS] = {0};
    int directory_entry_index;

    if (argc != 5) { printf("Usage: %s --input <image> --file <file>\n", argv[0]); return 1; }
    if (strcmp(argv[1], "--input") != 0 || strcmp(argv[3], "--file") != 0) { printf("Error: invalid command-line arguments.\n"); return 1; }
    image_name = argv[2]; source_name = argv[4];

    image = fopen(image_name, "rb+");
    if (!image) { printf("Error: file-system image not found.\n"); return 1; }
    fseek(image, SUPERBLOCK_BLOCK * BLOCK_SIZE, SEEK_SET);
    if (fread(&sb, sizeof(sb), 1, image) != 1) { printf("Error: could not read superblock.\n"); fclose(image); return 1; }
    if (sb.magic != MAGIC_NUMBER) { printf("Error: invalid SimpleFS image.\n"); fclose(image); return 1; }

    source = fopen(source_name, "rb");
    if (!source) { printf("Error: source file not found.\n"); fclose(image); return 1; }
    fseek(source, 0, SEEK_END); file_size = ftell(source); rewind(source);
    if (file_size < 0) { printf("Error: could not determine source file size.\n"); fclose(source); fclose(image); return 1; }
    if (file_size > MAX_FILE_SIZE) { printf("Error: file is too large for SimpleFS.\n"); fclose(source); fclose(image); return 1; }

    
    if (strlen(source_name) > 58) {
        printf("Error: file name is too long for SimpleFS.\n");
        fclose(source); fclose(image); return 1;
    }
    required_blocks = (int)((file_size + BLOCK_SIZE - 1) / BLOCK_SIZE);
    

    if (filename_exists(image, source_name)) { printf("Error: file already exists in SimpleFS.\n"); fclose(source); fclose(image); return 1; }

    fseek(image, INODE_BITMAP_BLOCK * BLOCK_SIZE, SEEK_SET);
    fread(inode_bitmap, BLOCK_SIZE, 1, image);
    free_inode = find_free_inode(inode_bitmap);
    if (free_inode == -1) { printf("Error: no free inode available.\n"); fclose(source); fclose(image); return 1; }

    fseek(image, DATA_BITMAP_BLOCK * BLOCK_SIZE, SEEK_SET);
    fread(data_bitmap, BLOCK_SIZE, 1, image);

    
    for (int i = 0; i < required_blocks; i++) {
        int block = find_free_data_block(data_bitmap);
        if (block == -1) {
            printf("Error: not enough free data blocks.\n");
            fclose(source); fclose(image); return 1;
        }
        set_bit(data_bitmap, data_bitmap_index(block));
        allocated_blocks[i] = block;
    }
    

    directory_entry_index = find_free_directory_entry(image);
    if (directory_entry_index == -1) { printf("Error: root directory is full.\n"); fclose(source); fclose(image); return 1; }

    
    {
        unsigned char buffer[BLOCK_SIZE];
        long remaining = file_size;
        for (int i = 0; i < required_blocks; i++) {
            long to_read = remaining < BLOCK_SIZE ? remaining : BLOCK_SIZE;
            memset(buffer, 0, BLOCK_SIZE);
            if (to_read > 0) {
                if (fread(buffer, 1, (size_t)to_read, source) != (size_t)to_read) {
                    printf("Error: could not read source file.\n");
                    fclose(source); fclose(image); return 1;
                }
            }
            fseek(image, (long)allocated_blocks[i] * BLOCK_SIZE, SEEK_SET);
            fwrite(buffer, BLOCK_SIZE, 1, image);
            remaining -= to_read;
        }
    }
   

    
    memset(&new_inode, 0, sizeof(new_inode));
    
    new_inode.type = TYPE_FILE;
    new_inode.links = 1;
    new_inode.size = (uint32_t)file_size;
    for (int i = 0; i < MAX_DIRECT_BLOCKS; i++) {
        new_inode.direct[i] = (i < required_blocks) ? (uint32_t)allocated_blocks[i] : 0;
    }
    
    fseek(image, inode_offset(free_inode), SEEK_SET);
    fwrite(&new_inode, sizeof(new_inode), 1, image);

    
    set_bit(inode_bitmap, free_inode - 1);
    
    fseek(image, INODE_BITMAP_BLOCK * BLOCK_SIZE, SEEK_SET);
    fwrite(inode_bitmap, BLOCK_SIZE, 1, image);
    fseek(image, DATA_BITMAP_BLOCK * BLOCK_SIZE, SEEK_SET);
    fwrite(data_bitmap, BLOCK_SIZE, 1, image);

    
    memset(&new_entry, 0, sizeof(new_entry));
    
    new_entry.inode_no = (uint32_t)free_inode;
    new_entry.type = TYPE_FILE;
    strncpy(new_entry.name, source_name, sizeof(new_entry.name) - 1);
    new_entry.name[sizeof(new_entry.name) - 1] = '\0';
    
    {
        long pos = ((long)ROOT_DATA_BLOCK * BLOCK_SIZE) + ((long)directory_entry_index * sizeof(dirent_t));
        fseek(image, pos, SEEK_SET);
        fwrite(&new_entry, sizeof(new_entry), 1, image);
    }

    fseek(image, inode_offset(ROOT_INODE), SEEK_SET);
    fread(&root_inode, sizeof(root_inode), 1, image);

    
    root_inode.size += (uint32_t)sizeof(dirent_t);
   
    fseek(image, inode_offset(ROOT_INODE), SEEK_SET);
    fwrite(&root_inode, sizeof(root_inode), 1, image);

    fclose(source); fclose(image);
    printf("%s added successfully to %s\n", source_name, image_name);
    return 0;
}
